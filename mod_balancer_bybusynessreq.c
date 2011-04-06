/* 
 * Copyright 2011 Toshiyuki Suzumura
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "httpd.h"
#include "http_config.h"
#include "mod_proxy.h"
#include "ap_mpm.h"
#include <curl/curl.h>

#define LBMETHOD "bybusynessreq"

#define AP_LOG_DEBUG(rec, fmt, ...) ap_log_error(APLOG_MARK, APLOG_DEBUG,  0, rec, fmt, ##__VA_ARGS__)
#define AP_LOG_INFO(rec, fmt, ...)  ap_log_error(APLOG_MARK, APLOG_INFO,   0, rec, "[" LBMETHOD "] " fmt, ##__VA_ARGS__)
#define AP_LOG_WARN(rec, fmt, ...)  ap_log_error(APLOG_MARK, APLOG_WARNING,0, rec, "[" LBMETHOD "] " fmt, ##__VA_ARGS__)
#define AP_LOG_ERR(rec, fmt, ...)   ap_log_error(APLOG_MARK, APLOG_ERR,    0, rec, "[" LBMETHOD "] " fmt, ##__VA_ARGS__)


module AP_MODULE_DECLARE_DATA balancer_bybusynessreq_module;

// Config store
typedef struct _lbmethod_conf {
  int timeout;    // HEAD request timeout by sec.
} lbmethod_conf;
static const lbmethod_conf DEFAULT_CONFIG = { 5 };


/*
 * heartbeat_handler for backend-apache.
 */
static int request_to_backend(proxy_worker* worker, server_rec* rec)
{
  int threaded_mpm;
  int code;
  CURLcode ret;
  CURL* curl = curl_easy_init();
  lbmethod_conf* conf = (lbmethod_conf*)ap_get_module_config(rec->module_config, &balancer_bybusynessreq_module);
  if(conf==NULL) {
    AP_LOG_WARN(rec, "config object not found.");
    conf = &DEFAULT_CONFIG;
  }

  ap_mpm_query(AP_MPMQ_IS_THREADED, &threaded_mpm);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, threaded_mpm);
  curl_easy_setopt(curl, CURLOPT_URL, worker->name);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "HEAD");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, conf->timeout);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, conf->timeout);
  ret = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);

  AP_LOG_DEBUG(rec, "checking backend server:'%s'=> %d, %d(%s)", worker->name, code, ret, curl_easy_strerror(ret));
  worker->s->error_time = apr_time_now();

  if((100<=code) && (code<500)) return OK;
  return DECLINED;
}


static int proxy_check_worker(const char *proxy_function, proxy_worker* worker, server_rec* rec)
{
  if(worker->s->status & PROXY_WORKER_IN_ERROR) {
    AP_LOG_DEBUG(rec, "proxy: %s: retrying the worker for (%s)", proxy_function, worker->hostname);
    long dif = (long)apr_time_now() - (long)worker->s->error_time;
    long lim = (long)worker->retry;
    AP_LOG_DEBUG(rec, "-- diff = %ld, retry = %ld", dif, (long)lim);
    if(dif > lim) {
      if(request_to_backend(worker, rec)==OK) {
        ++worker->s->retries;
        worker->s->status &= ~PROXY_WORKER_IN_ERROR;
        AP_LOG_DEBUG(rec, "proxy: %s: worker for (%s) has been marked for retry", proxy_function, worker->hostname);
        return OK;
      }
    }
    return DECLINED;
  }
  return OK;
}


/*
 * ProxySet lbmethod=bybusynessreq
 */
static proxy_worker *find_best_bybusyness_req(proxy_balancer* balancer, request_rec* rec)
{
  int i;
  proxy_worker *mycandidate = NULL;
  int cur_lbset = 0;
  int max_lbset = 0;
  int total_factor = 0;
  
  AP_LOG_DEBUG(rec->server, "proxy: Entering " LBMETHOD " for BALANCER (%s)", balancer->name);

  // First try to see if we have available candidate.
  do {
    int checking_standby = 0, checked_standby = 0;

    while(!mycandidate && !checked_standby) {
      proxy_worker *worker = (proxy_worker *)balancer->workers->elts;
      for(i = 0; i < balancer->workers->nelts; i++, worker++) {
        if(!checking_standby) {
          if(worker->s->lbset > max_lbset) max_lbset = worker->s->lbset;
        }
        if(worker->s->lbset != cur_lbset) continue;
        if( (checking_standby ? !PROXY_WORKER_IS_STANDBY(worker) : PROXY_WORKER_IS_STANDBY(worker)) ) continue;

        if(!PROXY_WORKER_IS_USABLE(worker)) proxy_check_worker("BALANCER", worker, rec->server);

        if(PROXY_WORKER_IS_USABLE(worker)) {
          worker->s->lbstatus += worker->s->lbfactor;
          total_factor += worker->s->lbfactor;
                    
          if(!mycandidate
              || worker->s->busy < mycandidate->s->busy
              || (worker->s->busy == mycandidate->s->busy && worker->s->lbstatus > mycandidate->s->lbstatus))
              mycandidate = worker;
        }
      }
      checked_standby = checking_standby++;
    }
    cur_lbset++;
  } while(cur_lbset <= max_lbset && !mycandidate);

  if(mycandidate) {
    mycandidate->s->lbstatus -= total_factor;
      AP_LOG_DEBUG(rec->server, "proxy: bybusyness selected worker \"%s\" : busy %" APR_SIZE_T_FMT " : lbstatus %d",
                     mycandidate->name, mycandidate->s->busy, mycandidate->s->lbstatus);
  }

  return mycandidate;
}


static const proxy_balancer_method bybusynessreq =
{
    LBMETHOD,
    &find_best_bybusyness_req,
    NULL
};

static void balancer_bybusynessreq_register_hook(apr_pool_t* pool)
{
  ap_register_provider(pool, PROXY_LBMETHOD, LBMETHOD, "0", &bybusynessreq);
}


static void* config_create(apr_pool_t* pool, char* d)
{
  lbmethod_conf* conf = (lbmethod_conf*)apr_palloc(pool, sizeof(lbmethod_conf));
  memcpy(conf, &DEFAULT_CONFIG, sizeof(*conf));
  return conf;
}

static const command_rec config_cmds[] = {
  AP_INIT_TAKE1(LBMETHOD "-timeout", ap_set_int_slot, \
    (void*)APR_OFFSETOF(lbmethod_conf, timeout), OR_OPTIONS, "HEAD request timeout."),
  { NULL }
};

module AP_MODULE_DECLARE_DATA balancer_bybusynessreq_module = {
    STANDARD20_MODULE_STUFF,
    config_create,  /* create per-directory config structure */
    NULL,           /* merge per-directory config structures */
    NULL,           /* create per-server config structure */
    NULL,           /* merge per-server config structures */
    config_cmds,    /* command apr_table_t */
    balancer_bybusynessreq_register_hook /* register hooks */
};
