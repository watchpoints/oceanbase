// Copyright (c) 2022-present Oceanbase Inc. All Rights Reserved.
// Author:
//   suzhi.yt <>

#define USING_LOG_PREFIX SERVER

#include "observer/table_load/ob_table_load_table_ctx.h"
#include "lib/allocator/ob_malloc.h"
#include "observer/table_load/ob_table_load_coordinator_ctx.h"
#include "observer/table_load/ob_table_load_store_ctx.h"
#include "observer/table_load/ob_table_load_task.h"
#include "observer/table_load/ob_table_load_task_scheduler.h"
#include "observer/table_load/ob_table_load_trans_ctx.h"
#include "observer/table_load/ob_table_load_utils.h"
#include "share/ob_common_rpc_proxy.h"

namespace oceanbase
{
namespace observer
{
using namespace common;
using namespace sql;
using namespace table;
using namespace obrpc;

ObTableLoadTableCtx::ObTableLoadTableCtx()
  : coordinator_ctx_(nullptr),
    store_ctx_(nullptr),
    job_stat_(nullptr),
    allocator_("TLD_TableCtx"),
    ref_count_(0),
    is_dirty_(false),
    is_inited_(false)
{
}

ObTableLoadTableCtx::~ObTableLoadTableCtx()
{
  destroy();
}

int ObTableLoadTableCtx::init(const ObTableLoadParam &param, const ObTableLoadDDLParam &ddl_param)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObTableLoadTableCtx init twice", KR(ret));
  } else if (OB_UNLIKELY(!param.is_valid() || !ddl_param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), K(param), K(ddl_param));
  } else {
    param_ = param;
    ddl_param_ = ddl_param;
    allocator_.set_tenant_id(MTL_ID());
    if (OB_FAIL(schema_.init(param_.tenant_id_, param_.table_id_))) {
      LOG_WARN("fail to init table load schema", KR(ret), K(param_.tenant_id_),
               K(param_.table_id_));
    } else if (OB_UNLIKELY(param.column_count_ != (schema_.is_heap_table_
                                                     ? (schema_.store_column_count_ - 1)
                                                     : schema_.store_column_count_))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected column count", KR(ret), K(param.column_count_), K(schema_.store_column_count_), K(schema_.is_heap_table_));
    } else if (OB_FAIL(task_allocator_.init("TLD_TaskPool", param_.tenant_id_))) {
      LOG_WARN("fail to init allocator", KR(ret));
    } else if (OB_FAIL(trans_ctx_allocator_.init("TLD_TCtxPool", param_.tenant_id_))) {
      LOG_WARN("fail to init allocator", KR(ret));
    } else if (OB_FAIL(register_job_stat())) {
      LOG_WARN("fail to register job stat", KR(ret));
    } else {
      is_inited_ = true;
    }
  }
  return ret;
}

int ObTableLoadTableCtx::register_job_stat()
{
  int ret = OB_SUCCESS;
  ObLoadDataStat *job_stat = nullptr;
  if (OB_ISNULL(job_stat = OB_NEWx(ObLoadDataStat, (&allocator_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to new ObLoadDataStat", KR(ret));
  } else {
    ObLoadDataGID temp_gid;
    ObLoadDataGID::generate_new_id(temp_gid);
    job_stat->tenant_id_ = param_.tenant_id_;
    job_stat->job_id_ = param_.table_id_;
    job_stat->job_type_ = "direct";
    job_stat->table_column_ = param_.column_count_;
    job_stat->batch_size_ = param_.batch_size_;
    job_stat->parallel_ = param_.session_count_;
    job_stat->start_time_ = ObTimeUtil::current_time();
    job_stat->max_allowed_error_rows_ = param_.max_error_row_count_;
    job_stat->detected_error_rows_ = 0;
    job_stat->coordinator.received_rows_ = 0;
    job_stat->coordinator.last_commit_segment_id_ = 0;
    job_stat->coordinator.status_ = "none";
    job_stat->coordinator.trans_status_ = "none";
    job_stat->store.processed_rows_ = 0;
    job_stat->store.last_commit_segment_id_ = 0;
    job_stat->store.status_ = "none";
    job_stat->store.trans_status_ = "none";
    job_stat->allocator_.set_tenant_id(param_.tenant_id_);
    if (OB_FAIL(ObTableLoadUtils::deep_copy(schema_.table_name_, job_stat->table_name_,
                                            job_stat->allocator_))) {
      LOG_WARN("fail to deep copy table name", KR(ret));
    } else if (OB_FAIL(ObGlobalLoadDataStatMap::getInstance()->register_job(temp_gid, job_stat))) {
      LOG_WARN("fail to register job stat", KR(ret));
    } else {
      gid_ = temp_gid;
      job_stat_ = job_stat;
      job_stat_->aquire();
    }
    if (OB_FAIL(ret)) {
      job_stat->~ObLoadDataStat();
      allocator_.free(job_stat);
      job_stat = nullptr;
    }
  }
  return ret;
}

void ObTableLoadTableCtx::unregister_job_stat()
{
  int ret = OB_SUCCESS;
  if (nullptr != job_stat_) {
    job_stat_->release();
    job_stat_ = nullptr;
  }
  if (gid_.is_valid()) {
    ObLoadDataStat *job_stat = nullptr;
    if (OB_FAIL(ObGlobalLoadDataStatMap::getInstance()->unregister_job(gid_, job_stat))) {
      LOG_ERROR("fail to unregister job stat", KR(ret), K_(gid));
    } else if (OB_ISNULL(job_stat)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("unexpected null job stat", KR(ret), K_(gid));
    } else {
      // 可能还有其他地方在引用这个对象, 等待释放引用
      int64_t log_print_cnt = 0;
      int64_t ref_cnt = 0;
      while ((ref_cnt = job_stat->get_ref_cnt()) > 0) {
        usleep(1L * 1000 * 1000);  // 1s
        if ((log_print_cnt++) % 10 == 0) {
          LOG_WARN("LOAD DATA wait job handle release", KR(ret), "wait_seconds", log_print_cnt * 10,
                   K_(gid), K(ref_cnt));
        }
      }
      job_stat->~ObLoadDataStat();
      allocator_.free(job_stat);
      job_stat = nullptr;
      gid_.reset();
    }
  }
}

int ObTableLoadTableCtx::init_coordinator_ctx(const ObIArray<int64_t> &idx_array, uint64_t user_id)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadTableCtx not init", KR(ret));
  } else if (OB_NOT_NULL(coordinator_ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("coordinator ctx already exist", KR(ret));
  } else {
    ObTableLoadCoordinatorCtx *coordinator_ctx = nullptr;
    if (OB_ISNULL(coordinator_ctx = OB_NEWx(ObTableLoadCoordinatorCtx, (&allocator_), this))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to new ObTableLoadCoordinatorCtx", KR(ret));
    } else if (OB_FAIL(coordinator_ctx->init(idx_array, user_id))) {
      LOG_WARN("fail to init coordinator ctx", KR(ret));
    } else if (OB_FAIL(coordinator_ctx->set_status_inited())) {
      LOG_WARN("fail to set coordinator status inited", KR(ret));
    } else {
      coordinator_ctx_ = coordinator_ctx;
    }
    if (OB_FAIL(ret)) {
      if (nullptr != coordinator_ctx) {
        coordinator_ctx->~ObTableLoadCoordinatorCtx();
        allocator_.free(coordinator_ctx);
        coordinator_ctx = nullptr;
      }
    }
  }
  return ret;
}

int ObTableLoadTableCtx::init_store_ctx(
  const ObTableLoadArray<ObTableLoadLSIdAndPartitionId> &partition_id_array,
  const ObTableLoadArray<ObTableLoadLSIdAndPartitionId> &target_partition_id_array)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadTableCtx not init", KR(ret));
  } else if (OB_NOT_NULL(store_ctx_)) {
    ret = OB_ENTRY_EXIST;
    LOG_WARN("store ctx already exist", KR(ret));
  } else {
    ObTableLoadStoreCtx *store_ctx = nullptr;
    if (OB_ISNULL(store_ctx = OB_NEWx(ObTableLoadStoreCtx, (&allocator_), this))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to new ObTableLoadStoreCtx", KR(ret));
    } else if (OB_FAIL(store_ctx->init(partition_id_array, target_partition_id_array))) {
      LOG_WARN("fail to init store ctx", KR(ret));
    } else if (OB_FAIL(store_ctx->set_status_inited())) {
      LOG_WARN("fail to set store status inited", KR(ret));
    } else {
      store_ctx_ = store_ctx;
    }
    if (OB_FAIL(ret)) {
      if (nullptr != store_ctx) {
        store_ctx->~ObTableLoadStoreCtx();
        allocator_.free(store_ctx);
        store_ctx = nullptr;
      }
    }
  }
  return ret;
}

void ObTableLoadTableCtx::stop()
{
  if (nullptr != coordinator_ctx_) {
    coordinator_ctx_->stop();
  }
  if (nullptr != store_ctx_) {
    store_ctx_->stop();
  }
  LOG_INFO("ctx stop succ");
}

void ObTableLoadTableCtx::destroy()
{
  abort_unless(0 == get_ref_count());
  if (nullptr != coordinator_ctx_) {
    coordinator_ctx_->~ObTableLoadCoordinatorCtx();
    allocator_.free(coordinator_ctx_);
    coordinator_ctx_ = nullptr;
  }
  if (nullptr != store_ctx_) {
    store_ctx_->~ObTableLoadStoreCtx();
    allocator_.free(store_ctx_);
    store_ctx_ = nullptr;
  }
  unregister_job_stat();
  is_inited_ = false;
}

int ObTableLoadTableCtx::alloc_task(ObTableLoadTask *&task)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadTableCtx not init", KR(ret));
  } else {
    if (OB_ISNULL(task = task_allocator_.alloc(param_.tenant_id_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to alloc task", KR(ret));
    }
  }
  return ret;
}

void ObTableLoadTableCtx::free_task(ObTableLoadTask *task)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadTableCtx not init", KR(ret));
  } else if (OB_ISNULL(task)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid null task", KR(ret));
  } else {
    task_allocator_.free(task);
  }
}

ObTableLoadTransCtx *ObTableLoadTableCtx::alloc_trans_ctx(const ObTableLoadTransId &trans_id)
{
  int ret = OB_SUCCESS;
  ObTableLoadTransCtx *trans_ctx = nullptr;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadTableCtx not init", KR(ret));
  } else {
    trans_ctx = trans_ctx_allocator_.alloc(this, trans_id);
  }
  return trans_ctx;
}

void ObTableLoadTableCtx::free_trans_ctx(ObTableLoadTransCtx *trans_ctx)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadTableCtx not init", KR(ret));
  } else if (OB_ISNULL(trans_ctx)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid null trans ctx", KR(ret));
  } else {
    trans_ctx_allocator_.free(trans_ctx);
  }
}

}  // namespace observer
}  // namespace oceanbase
