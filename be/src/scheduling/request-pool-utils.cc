// Copyright 2014 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "scheduling/request-pool-utils.h"

#include <list>
#include <string>

#include "common/logging.h"
#include "rpc/thrift-util.h"
#include "util/jni-util.h"
#include "util/parse-util.h"

using namespace std;
using namespace impala;

DEFINE_string(fair_scheduler_allocation_path, "", "Path to the fair scheduler "
    "allocation file (fair-scheduler.xml), either an absolute path or a relative path "
    "on the classpath.");
DEFINE_string(llama_site_path, "", "Path to the Llama configuration file "
    "(llama-site.xml), either an absolute path or a relative path on the classpath. If "
    "set, fair_scheduler_allocation_path must also be set.");

// The default_pool parameters are used if fair scheduler allocation and Llama
// configuration files are not provided.
DEFINE_int64(default_pool_max_requests, -1, "Maximum number of concurrent outstanding "
    "requests allowed to run before queueing incoming requests. A negative value "
    "indicates no limit. Ignored if fair_scheduler_config_path and "
    "llama_site_path are set.");
DEFINE_string(default_pool_mem_limit, "", "Maximum amount of memory that all "
    "outstanding requests in this pool may use before new requests to this pool"
    " are queued. Specified as a number of bytes ('<int>[bB]?'), megabytes "
    "('<float>[mM]'), gigabytes ('<float>[gG]'), or percentage of the physical memory "
    "('<int>%'). -1 or not setting indicates no limit. Defaults to bytes if no unit is "
    "given. Ignored if fair_scheduler_config_path and llama_site_path are set.");
DEFINE_int64(default_pool_max_queued, 0, "Maximum number of requests allowed to be "
    "queued before rejecting requests. A negative value or 0 indicates requests "
    "will always be rejected once the maximum number of concurrent requests are "
    "executing. Ignored if fair_scheduler_config_path and "
    "llama_site_path are set.");

// Pool name used when the configuration files are not specified.
const string DEFAULT_POOL_NAME = "default-pool";

RequestPoolUtils::RequestPoolUtils() {
  if (FLAGS_fair_scheduler_allocation_path.empty() &&
      FLAGS_llama_site_path.empty()) {
    default_pool_only_ = true;
    bool is_percent; // not used
    int64_t bytes_limit = ParseUtil::ParseMemSpec(FLAGS_default_pool_mem_limit,
        &is_percent);
    // -1 indicates an error occurred
    if (bytes_limit < 0) {
      LOG(ERROR) << "Unable to parse default pool mem limit from '"
                 << FLAGS_default_pool_mem_limit << "'.";
      exit(1);
    }
    // 0 indicates no limit or not set
    if (bytes_limit == 0) {
      default_pool_mem_limit_ = -1;
    } else {
      default_pool_mem_limit_ = bytes_limit;
    }
    return;
  }
  default_pool_only_ = false;

  jmethodID start_id; // RequestPoolUtils.start(), only called in this method.
  JniMethodDescriptor methods[] = {
    {"<init>", "(Ljava/lang/String;Ljava/lang/String;)V", &ctor_},
    {"start", "()V", &start_id},
    {"resolveRequestPool", "([B)[B", &resolve_request_pool_id_},
    {"getPoolConfig", "([B)[B", &get_pool_config_id_}};

  JNIEnv* jni_env = getJNIEnv();
  pool_utils_class_ = jni_env->FindClass("com/cloudera/impala/util/RequestPoolUtils");
  uint32_t num_methods = sizeof(methods) / sizeof(methods[0]);
  for (int i = 0; i < num_methods; ++i) {
    EXIT_IF_ERROR(JniUtil::LoadJniMethod(jni_env, pool_utils_class_, &(methods[i])));
  }

  jstring fair_scheduler_config_path =
      jni_env->NewStringUTF(FLAGS_fair_scheduler_allocation_path.c_str());
  EXIT_IF_EXC(jni_env);
  jstring llama_site_path =
      jni_env->NewStringUTF(FLAGS_llama_site_path.c_str());
  EXIT_IF_EXC(jni_env);

  jobject pool_utils = jni_env->NewObject(pool_utils_class_, ctor_,
      fair_scheduler_config_path, llama_site_path);
  EXIT_IF_EXC(jni_env);
  EXIT_IF_ERROR(JniUtil::LocalToGlobalRef(jni_env, pool_utils, &pool_utils_));
  jni_env->CallObjectMethod(pool_utils_, start_id);
  EXIT_IF_EXC(jni_env);
}

Status RequestPoolUtils::ResolveRequestPool(const string& pool, const string& user,
    TResolveRequestPoolResult* resolved_pool) {
  if (default_pool_only_) {
    resolved_pool->__set_resolved_pool(DEFAULT_POOL_NAME);
    resolved_pool->__set_has_access(true);
    return Status::OK;
  }

  TResolveRequestPoolParams params;
  params.__set_user(user);
  params.__set_requested_pool(pool);
  return JniUtil::CallJniMethod(pool_utils_, resolve_request_pool_id_, params,
      resolved_pool);
}

Status RequestPoolUtils::GetPoolConfig(const string& pool,
    TPoolConfigResult* pool_config) {
  if (default_pool_only_) {
    pool_config->__set_max_requests(FLAGS_default_pool_max_requests);
    pool_config->__set_max_queued(FLAGS_default_pool_max_queued);
    pool_config->__set_mem_limit(default_pool_mem_limit_);
    return Status::OK;
  }

  TPoolConfigParams params;
  params.__set_pool(pool);
  return JniUtil::CallJniMethod(pool_utils_, get_pool_config_id_, params, pool_config);
}
