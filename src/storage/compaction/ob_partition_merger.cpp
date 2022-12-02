/**
 	* Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX STORAGE_COMPACTION
#include "ob_partition_merger.h"
#include "lib/file/file_directory_utils.h"
#include "logservice/ob_log_service.h"
#include "ob_tenant_tablet_scheduler.h"
#include "ob_tablet_merge_task.h"
#include "ob_tablet_merge_ctx.h"
#include "ob_i_compaction_filter.h"
#include "storage/tx/ob_trans_service.h"

namespace oceanbase
{
using namespace share::schema;
using namespace common;
using namespace memtable;
using namespace storage;
using namespace blocksstable;

namespace compaction
{
/*
 * Misc
 */

static inline bool macro_cmp_ret_valid(const int cmp_ret)
{
  return cmp_ret <= ObPartitionMergeIter::CANNOT_COMPARE_RIGHT_IS_RANGE
         && cmp_ret >= ObPartitionMergeIter::CANNOT_COMPARE_LEFT_IS_RANGE;
}

static inline bool macro_need_open_left(const int cmp_ret)
{
  return ObPartitionMergeIter::CANNOT_COMPARE_LEFT_IS_RANGE == cmp_ret;
}

static inline bool macro_need_open_right(const int cmp_ret)
{
  return ObPartitionMergeIter::CANNOT_COMPARE_RIGHT_IS_RANGE == cmp_ret;
}

static inline bool macro_need_open(const int cmp_ret)
{
  return macro_need_open_left(cmp_ret) || macro_need_open_right(cmp_ret);
}

/*
 *ObPartitionMerger
 */

ObPartitionMerger::ObPartitionMerger()
  : allocator_("partMerger"),
    merge_ctx_(nullptr),
    merge_progress_(nullptr),
    partition_fuser_(nullptr),
    data_store_desc_(),
    merge_info_(),
    macro_writer_(),
    minimum_iters_(DEFAULT_ITER_ARRAY_SIZE, ModulePageAllocator(allocator_)),
    task_idx_(0),
    is_inited_(false)
{
}

ObPartitionMerger::~ObPartitionMerger()
{
  reset();
}

void ObPartitionMerger::reset()
{
  is_inited_ = false;
  task_idx_ = 0;
  minimum_iters_.reset();
  macro_writer_.reset();
  merge_info_.reset();
  data_store_desc_.reset();
  merge_ctx_ = nullptr;
  if (OB_NOT_NULL(partition_fuser_)) {
    partition_fuser_->~ObIPartitionMergeFuser();
    allocator_.free(partition_fuser_);
    partition_fuser_ = nullptr;
  }
  allocator_.reset();
}

int ObPartitionMerger::init_data_store_desc(ObTabletMergeCtx &ctx)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(data_store_desc_.init(*ctx.get_merge_schema(),
                                    ctx.param_.ls_id_,
                                    ctx.param_.tablet_id_,
                                    ctx.param_.merge_type_,
                                    ctx.sstable_version_range_.snapshot_version_))) {
    STORAGE_LOG(WARN, "Failed to init data store desc", K(ret), K(ctx));
  } else {
    merge_info_.reset();
    merge_info_.tenant_id_ = MTL_ID();
    merge_info_.ls_id_ = ctx.param_.ls_id_;
    merge_info_.tablet_id_ = ctx.param_.tablet_id_;
    merge_info_.merge_start_time_ = ObTimeUtility::fast_current_time();
    merge_info_.merge_type_ = ctx.param_.merge_type_;
    merge_info_.compaction_scn_ = ctx.get_compaction_scn();
    merge_info_.progressive_merge_round_ = ctx.progressive_merge_round_;
    merge_info_.progressive_merge_num_ = ctx.progressive_merge_num_;
    merge_info_.concurrent_cnt_ = ctx.parallel_merge_ctx_.get_concurrent_cnt();
    merge_info_.is_full_merge_ = ctx.is_full_merge_;
    data_store_desc_.merge_info_ = &merge_info_;
  }
  return ret;
}

int ObPartitionMerger::open_macro_writer(ObMergeParameter &merge_param)
{
  int ret = OB_SUCCESS;
  ObITable *table = nullptr;
  ObSSTable *first_sstable = nullptr;
  ObMacroDataSeq macro_start_seq(0);

  if (OB_UNLIKELY(!merge_param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid merge parameter", K(ret), K(merge_param));
  } else if (OB_ISNULL(table = merge_ctx_->tables_handle_.get_tables().at(0))) {
    ret = OB_ERR_SYS;
    STORAGE_LOG(WARN, "sstable is null", K(ret));
  } else if (!table->is_sstable() && merge_ctx_->param_.is_major_merge()) {
    ret = OB_ERR_SYS;
    STORAGE_LOG(WARN, "Unexpected first table for major merge", K(ret), KPC(merge_ctx_));
  } else if (OB_FAIL(macro_start_seq.set_parallel_degree(task_idx_))) {
    STORAGE_LOG(WARN, "Failed to set parallel degree to macro start seq", K(ret), K_(task_idx));
  } else {
    data_store_desc_.end_scn_ = merge_ctx_->scn_range_.end_scn_;
    if (OB_FAIL(macro_writer_.open(data_store_desc_, macro_start_seq))) {
      STORAGE_LOG(WARN, "Failed to open macro block writer", K(ret));
    }
  }
  return ret;
}

int ObPartitionMerger::close()
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObPartitionMerger is not inited", K(ret), K(*this));
  } else if (OB_FAIL(macro_writer_.close())) {
    STORAGE_LOG(WARN, "Failed to close macro block writer", K(ret));
  } else {
    ObTabletMergeInfo &merge_info = merge_ctx_->get_merge_info();
    merge_info_.merge_finish_time_ = common::ObTimeUtility::fast_current_time();
    if (OB_FAIL(merge_info.add_macro_blocks(task_idx_,
                                               &(macro_writer_.get_macro_block_write_ctx()),
                                               merge_info_))) {
      STORAGE_LOG(WARN, "Failed to add macro blocks", K(ret));
    } else {
      merge_info_.dump_info("macro block builder close");
    }
  }

  return ret;
}

int ObPartitionMerger::check_row_columns(const ObDatumRow &row)
{
  int ret = OB_SUCCESS;
  if (row.row_flag_.is_not_exist() || row.row_flag_.is_delete()) {
  } else if (row.count_ != data_store_desc_.row_column_count_) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(ERROR, "Unexpected column count of store row", K(row), K_(data_store_desc), K(ret));
  }
  return ret;
}

int ObPartitionMerger::process(const ObMacroBlockDesc &macro_desc)
{
  int ret = OB_SUCCESS;

#ifdef ERRSIM
  int64_t macro_block_builder_errsim_flag = GCONF.macro_block_builder_errsim_flag;
  if (2 == macro_block_builder_errsim_flag) {
    if (macro_writer_.get_macro_block_write_ctx().get_macro_block_count() -
        merge_info_.multiplexed_macro_block_count_ >= 1) {
      ret = OB_ERR_SYS;
      STORAGE_LOG(ERROR, "fake macro_block_builder_errsim_flag", K(ret),
                  K(macro_block_builder_errsim_flag));
    }
  }
#endif

  if (OB_FAIL(ret)) {
    // do nothing
  } else if (OB_UNLIKELY(!macro_desc.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument to append macro block", K(ret), K(macro_desc));
  } else if (OB_FAIL(macro_writer_.append_macro_block(macro_desc))) {
    LOG_WARN("Failed to append to macro block writer", K(ret));
  } else {
    LOG_DEBUG("Success to append macro block", K(ret), K(macro_desc));
  }
  return ret;
}

int ObPartitionMerger::process(const ObMicroBlock &micro_block)
{
  int ret = OB_SUCCESS;

  if (!micro_block.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "invalid argument to append micro block", K(ret), K(micro_block));
  } else if (OB_FAIL(macro_writer_.append_micro_block(micro_block))) {
    STORAGE_LOG(WARN, "Failed to append micro block to macro block writer", K(ret), K(micro_block));
  }

  return ret;
}

int ObPartitionMerger::process(const ObDatumRow &row)
{
  int ret = OB_SUCCESS;
  ObICompactionFilter::ObFilterRet filter_ret = ObICompactionFilter::FILTER_RET_MAX;
#ifdef ERRSIM
  int64_t macro_block_builder_errsim_flag = GCONF.macro_block_builder_errsim_flag;
  if (1 == macro_block_builder_errsim_flag) {
    if (macro_writer_.get_macro_block_write_ctx().get_macro_block_count() > 1) {
      ret = OB_ERR_SYS;
      STORAGE_LOG(ERROR, "fake macro_block_builder_errsim_flag", K(ret),
                  K(macro_block_builder_errsim_flag));
    }
  }
#endif

  if (OB_FAIL(ret)) {
    // fake errsim
  } else if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObPartitionMerger is not inited", K(ret), K(*this));
  } else if (OB_UNLIKELY(!row.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to append row", K(ret), K(row));
  } else if (row.row_flag_.is_not_exist()) {
    STORAGE_LOG(ERROR, "Unexpected not exist row to append", K(ret), K(row));
  } else if (OB_FAIL(try_filter_row(row, filter_ret))) {
    STORAGE_LOG(WARN, "failed to filter row", K(ret), K(row));
  } else if (ObICompactionFilter::FILTER_RET_REMOVE == filter_ret) {
    // drop this row
  } else if (OB_FAIL(check_row_columns(row))) {
    STORAGE_LOG(WARN, "Failed to check row columns", K(ret), K(row));
  } else if (OB_FAIL(inner_process(row))) {
    STORAGE_LOG(WARN, "Failed to inner append row", K(ret));
  }
  return ret;
}

int ObPartitionMerger::try_filter_row(
    const ObDatumRow &row,
    ObICompactionFilter::ObFilterRet &filter_ret)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(merge_ctx_->compaction_filter_)) {
    if (OB_FAIL(merge_ctx_->compaction_filter_->filter(
        row,
        filter_ret))) {
      STORAGE_LOG(WARN, "failed to filter row", K(ret), K(filter_ret));
    } else if (OB_UNLIKELY(filter_ret >= ObICompactionFilter::FILTER_RET_MAX
        || filter_ret < ObICompactionFilter::FILTER_RET_NOT_CHANGE)) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "get wrong filter ret", K(filter_ret));
    } else {
      merge_info_.filter_statistics_.inc(filter_ret);
    }
  }
  return ret;
}

int ObPartitionMerger::prepare_merge_partition(ObMergeParameter &merge_param,
                                               MERGE_ITER_ARRAY &merge_iters)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObPartitionMerger is not inited", K(ret));
  } else if (OB_FAIL(merge_param.init(*merge_ctx_, task_idx_))) {
    STORAGE_LOG(WARN, "Failed to assign the merge param", K(ret), KPC(merge_ctx_), K_(task_idx));
  } else if (OB_UNLIKELY(!merge_param.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected invalid merge param", K(ret), K(merge_param));
  } else if (OB_FAIL(open_macro_writer(merge_param))) {
    STORAGE_LOG(WARN, "Failed to open macro writer", K(ret), K(merge_param));
  } else if (OB_FAIL(init_partition_fuser(merge_param))) {
    STORAGE_LOG(WARN, "Failed to init partition merge fuser", K(merge_param), K(ret));
  } else if (OB_FAIL(init_merge_iters(*partition_fuser_, merge_param, merge_iters))) {
    STORAGE_LOG(WARN, "Failed to init merge iters", K(ret));
  }

  return ret;
}

int ObPartitionMerger::end_merge_partition(MERGE_ITER_ARRAY &merge_iters)
{
  int ret = OB_SUCCESS;
  ObPartitionMergeIter *iter = nullptr;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObPartitionMerger is not inited", K(ret));
  } else {
    // check the validness of the txn table for possible concurrent rebuild or server stop
    for (int64_t i = 0; OB_SUCC(ret) && i < merge_iters.count(); ++i) {
      if (OB_ISNULL(iter = merge_iters.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected null merge iter", K(ret), K(i), K(merge_iters));
      } else if (!iter->is_iter_end()) {
        ret = OB_ERR_SYS;
        STORAGE_LOG(ERROR, "Merge iter not iter to end", K(ret), KPC(iter));
      } else if (i == 0 && !iter->is_tx_table_valid()) {
        ret = OB_STATE_NOT_MATCH;
        STORAGE_LOG(ERROR, "Failed to complete the merge because of broken txn table", K(ret), KPC(iter));
      }
    }
    if (OB_FAIL(ret)) {
      if (GCONF._enable_compaction_diagnose) {
        ObPartitionMergeDumper::print_error_info(ret, *merge_ctx_, merge_iters);
        macro_writer_.dump_block_and_writer_buffer();
      }
    } else if (OB_FAIL(close())) {
      STORAGE_LOG(WARN, "Fail to close partition merger", K(ret));
    }
  }

  return ret;
}

void ObPartitionMerger::clean_iters_and_reset(MERGE_ITER_ARRAY &merge_iters)
{
  ObPartitionMergeIter *iter = nullptr;
  const ObITable *table = nullptr;
  for (int64_t i = 0; i < merge_iters.count(); ++i) {
    if (OB_NOT_NULL(iter = merge_iters.at(i))) {
      if (OB_NOT_NULL(table = iter->get_table())) {
        FLOG_INFO("partition merge iter row count", K(i), "row_count", iter->get_iter_row_count(),
            "ghost_row_count", iter->get_ghost_row_count(), "pkey", table->get_key(), KPC(table));
      }
      iter->~ObPartitionMergeIter();
    }
  }
  merge_iters.reset();
  if (OB_NOT_NULL(partition_fuser_)) {
    partition_fuser_->~ObIPartitionMergeFuser();
    partition_fuser_ = nullptr;
  }
  reset();
}

int ObPartitionMerger::move_iters_next(MERGE_ITER_ARRAY &merge_iters)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < merge_iters.count(); ++i) {
    if (OB_ISNULL(merge_iters.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "Unexpected null merge iter", K(ret), K(i), K(merge_iters));
    } else if (OB_FAIL(merge_iters.at(i)->next())) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      } else {
        STORAGE_LOG(WARN, "Fail to next merge iter", K(i), K(ret), KPC(merge_iters.at(i)));
      }
    }
  }
  return ret;
}


template <typename T> T *ObPartitionMerger::alloc_merge_helper()
{
  void *buf = nullptr;
  T *merge_iter = nullptr;
  if (OB_ISNULL(buf = allocator_.alloc(sizeof(T)))) {
  } else {
    merge_iter = new (buf) T();
  }
  return merge_iter;
}

int ObPartitionMerger::compare_row_iters_simple(ObPartitionMergeIter *base_iter,//fuse base iter == newest iter
                                                ObPartitionMergeIter *macro_row_iter,//current iter
                                                int &cmp_ret)
{
  int ret = OB_SUCCESS;
  cmp_ret = 0;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObPartitionMerger is not inited", K_(is_inited), K(ret));
  } else if (OB_ISNULL(macro_row_iter) || OB_ISNULL(base_iter)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid arguments to compare row iters", KP(macro_row_iter),
                KP(base_iter), K(ret));
  } else if (OB_UNLIKELY(macro_row_iter->is_iter_end() || base_iter->is_iter_end())) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected end row iters", K(ret));
  } else if (OB_FAIL(macro_row_iter->compare(*base_iter, cmp_ret))) {
    STORAGE_LOG(WARN, "Failed to compare macro_row_iter", K(ret));
  } else if (!macro_cmp_ret_valid(cmp_ret)) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected macro compare result", K(cmp_ret), K(ret));
  }

  return ret;
}

int ObPartitionMerger::compare_row_iters_range(ObPartitionMergeIter *base_iter,//fuse base iter == newest iter
                                               ObPartitionMergeIter *macro_row_iter,//current iter
                                               int &cmp_ret)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObPartitionMerger  is not inited", K_(is_inited), K(ret));
  } else if (OB_ISNULL(macro_row_iter) || OB_ISNULL(base_iter)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid arguments to compare rowiters", KP(macro_row_iter),
                KP(base_iter), K(ret));
  } else if (!macro_need_open(cmp_ret)) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected intial compare result", K(cmp_ret), K(ret));
  } else {
    while (OB_SUCC(ret) && macro_need_open(cmp_ret)) {
      if (macro_need_open_left(cmp_ret) && OB_FAIL(macro_row_iter->open_curr_range(false /*for_rewrite*/))) {
        if (OB_LIKELY(OB_ITER_END == ret)) {
          ret = OB_SUCCESS;
          cmp_ret = 1;
        } else {
          STORAGE_LOG(WARN, "Fail to open current range", K(ret));
        }
      }

      if (OB_SUCC(ret)) {
        if (macro_need_open_right(cmp_ret) && OB_FAIL(base_iter->open_curr_range(false /*for_rewrite*/))) {
          if (OB_LIKELY(OB_ITER_END == ret)) {
            ret = OB_SUCCESS;
            cmp_ret = -1;
          } else {
            STORAGE_LOG(WARN, "Fail to open current range", K(ret));
          }
        }
      }
      if (OB_SUCC(ret) && macro_need_open(cmp_ret)) {
        if (OB_FAIL(macro_row_iter->compare(*base_iter, cmp_ret))) {
          STORAGE_LOG(WARN, "Failed to compare merge_row_iter", K(ret));
        } else if (!macro_cmp_ret_valid(cmp_ret)) {
          ret = OB_ERR_UNEXPECTED;
          STORAGE_LOG(WARN, "Unexpected macro compare result", K(cmp_ret), K(ret));
        }
      }
    }
  }

  return ret;
}



/*
 *ObPartitionMajorMerger
 */
ObPartitionMajorMerger::ObPartitionMajorMerger()
{
}

ObPartitionMajorMerger::~ObPartitionMajorMerger()
{
}

int ObPartitionMajorMerger::open(ObTabletMergeCtx &ctx, const int64_t idx)
{
  int ret = OB_SUCCESS;

  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    STORAGE_LOG(WARN, "ObPartitionMerger is init twice", K(ret), K(*this));
  } else if (OB_UNLIKELY(!ctx.is_valid() || idx < 0)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to init ObPartitionMerger", K(ret), K(ctx), K(idx));
  } else if (OB_FAIL(init_data_store_desc(ctx))) {
    STORAGE_LOG(WARN, "Failed to init data store desc", K(ret));
  } else {
    merge_ctx_ = &ctx;
    if (OB_NOT_NULL(ctx.merge_progress_)) {
      merge_progress_ = ctx.merge_progress_;
    }
    task_idx_ = idx;
    data_store_desc_.sstable_index_builder_ = ctx.get_merge_info().get_index_builder();
    is_inited_ = true;
  }

  return ret;
}

int ObPartitionMajorMerger::inner_process(const ObDatumRow &row)
{
  int ret = OB_SUCCESS;
  const bool is_delete = row.row_flag_.is_delete();
  if (is_delete) {
      // drop del row
  } else if (OB_FAIL(macro_writer_.append_row(row))) {
    STORAGE_LOG(WARN, "Failed to append row to macro writer", K(ret));
  }

  if (OB_SUCC(ret)) {
    STORAGE_LOG(DEBUG, "Success to virtual append row to major macro writer", K(ret), K(row));
  }
  return ret;
}

int ObPartitionMajorMerger::init_partition_fuser(const ObMergeParameter &merge_param)
{
  int ret = OB_SUCCESS;
  partition_fuser_ = nullptr;

  if (merge_param.is_buf_minor_merge()) {
    partition_fuser_ = alloc_merge_helper<ObBufPartitionMergeFuser>();
  } else {
    partition_fuser_ = alloc_merge_helper<ObMajorPartitionMergeFuser>();
  }
  if (OB_ISNULL(partition_fuser_)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    STORAGE_LOG(WARN, "Failed to allocate memory for partition fuser", K(ret), K(merge_param));
  } else if (OB_FAIL(partition_fuser_->init(merge_param))) {
    STORAGE_LOG(WARN, "Failed to init partition fuser", K(ret));
  }

  if (OB_FAIL(ret)) {
    if (OB_NOT_NULL(partition_fuser_)) {
      partition_fuser_->~ObIPartitionMergeFuser();
      partition_fuser_ = nullptr;
    }
  }

  return ret;
}


int ObPartitionMajorMerger::init_merge_iters(ObIPartitionMergeFuser &fuser,
                                             ObMergeParameter &merge_param,
                                             MERGE_ITER_ARRAY &merge_iters)
{
  int ret = OB_SUCCESS;
  ObPartitionMergeIter *merge_iter = nullptr;
  ObITable *table = nullptr;
  ObSSTable *sstable = nullptr;
  merge_iters.reset();

  for (int64_t i = 0; OB_SUCC(ret) && i < merge_param.tables_handle_->get_count(); i++) {
    if (OB_ISNULL(table = merge_param.tables_handle_->get_table(i))) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "Unexpected null table", K(ret), K(i), K(merge_param));
    } else if (OB_UNLIKELY(!table->is_sstable())) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "Unexpected table type for major merge", K(ret), K(i), KPC(table),
                  KPC(merge_param.tables_handle_));
    } else if (FALSE_IT(sstable = static_cast<ObSSTable *>(table))) {
    } else if (OB_UNLIKELY(sstable->is_remote_logical_minor_sstable())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected remote minor sstable", K(ret), KP(sstable));
    } else if (sstable->get_meta().get_basic_meta().get_data_macro_block_count() <= 0) {
      // do nothing. don't need to construct iter for empty sstable
      FLOG_INFO("table is empty, need not create iter", K(i), KPC(sstable), K(sstable->get_meta()));
      continue;
    } else if (0 == i && !merge_param.is_full_merge_ && !sstable->is_small_sstable()) {
      if (MICRO_BLOCK_MERGE_LEVEL == merge_param.merge_level_) {
        merge_iter = alloc_merge_helper<ObPartitionMicroMergeIter>();
      } else {
        merge_iter = alloc_merge_helper<ObPartitionMacroMergeIter>();
      }
    } else {
      merge_iter = alloc_merge_helper<ObPartitionRowMergeIter>();
    }
    if (OB_SUCC(ret)) {
      if (OB_ISNULL(merge_iter)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        STORAGE_LOG(WARN, "Failed to alloc memory for merge iter", K(ret));
      } else if (OB_FAIL(merge_iter->init(merge_param,
              fuser.get_multi_version_column_ids(),
              data_store_desc_.row_store_type_, i))) {
        if (OB_UNLIKELY(OB_BEYOND_THE_RANGE != ret)) {
          STORAGE_LOG(WARN, "Failed to init merge iter", K(ret), K(i));
        } else {
          ObITable *table = merge_param.tables_handle_->get_table(i);
          STORAGE_LOG(WARN, "Ignore sstable beyond the range", K(i), K(merge_param.merge_range_),
                      KPC(table));
          merge_iter->~ObPartitionMergeIter();
          merge_iter = nullptr;
          ret = OB_SUCCESS;
        }
      } else if (OB_FAIL(merge_iters.push_back(merge_iter))) {
        STORAGE_LOG(WARN, "Failed to push back merge iter", K(ret), KPC(merge_iter));
      } else {
        merge_iter = nullptr;
      }
    }

    if (OB_FAIL(ret)) {
      if (nullptr != merge_iter) {
        merge_iter->~ObPartitionMergeIter();
        merge_iter = nullptr;
      }
    }
  }

  return ret;
}

int ObPartitionMajorMerger::merge_partition(ObTabletMergeCtx &ctx, const int64_t idx)
{
  int ret = OB_SUCCESS;
  MERGE_ITER_ARRAY merge_iters;
  ObMergeParameter merge_param;
  int64_t need_rewrite_block_cnt = 0;
  int64_t rewrite_block_cnt = 0;

  if (OB_FAIL(open(ctx, idx))) {
    STORAGE_LOG(WARN, "Failed to open partition major merge fuse", K(ret));
  } else if (OB_FAIL(prepare_merge_partition(merge_param, merge_iters))) {
    STORAGE_LOG(WARN, "Failed to prepare merge partition", K(ret));
  } else if (OB_ISNULL(partition_fuser_)) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected null partition fuser", K(ret));
  } else {
    if (merge_iters.empty()) {
      ret = OB_ITER_END;
    } else if (merge_param.is_major_merge()
        && OB_FAIL(get_macro_block_count_to_rewrite(merge_param.merge_range_, need_rewrite_block_cnt))) {
      STORAGE_LOG(WARN, "Failed to compute the count of macro block to rewrite", K(ret));
    } else if (OB_FAIL(move_iters_next(merge_iters))) {
      STORAGE_LOG(WARN, "Failed to move merge iters next", K(ret));
    } else {
      int64_t reuse_row_cnt = 0;
      bool is_reuse_base_sstable = false;
      if (OB_NOT_NULL(merge_iters.at(0)) && !merge_iters.at(0)->is_base_sstable_iter()) {
        //if base iter does not exist ,not reuse_base_sstable
      } else if (OB_FAIL(check_need_reuse_base_sstable(merge_iters, need_rewrite_block_cnt,
        merge_param.is_full_merge_, is_reuse_base_sstable))) {
        STORAGE_LOG(WARN, "Failed check_need_reuse_base_sstable", K(ret));
      } else if (is_reuse_base_sstable) {
        if (OB_FAIL(reuse_base_sstable(merge_iters)) && OB_ITER_END != ret) {
          STORAGE_LOG(WARN, "Failed to reuse base sstable", K(ret), K(merge_iters));
        } else {
          FLOG_INFO("succeed to reuse base sstable", K(merge_iters));
        }
      }

      while (OB_SUCC(ret)) {
        share::dag_yield();
        //find minimum merge iter
        if (OB_UNLIKELY(!MTL(ObTenantTabletScheduler *)->could_major_merge_start())) {
          ret = OB_CANCELED;
          STORAGE_LOG(WARN, "Major merge has been paused", K(ret));
        } else if (OB_FAIL(find_minimum_iters(merge_iters, minimum_iters_))) {
          STORAGE_LOG(WARN, "Failed to find minimum iters", K(ret));
        } else {
          bool rewrite = false;
          if (0 == minimum_iters_.count()) {
            ret = OB_ITER_END;
          } else if (1 == minimum_iters_.count() && nullptr == minimum_iters_.at(0)->get_curr_row()) {
            ObPartitionMergeIter *iter = minimum_iters_.at(0);
            if (!iter->is_macro_block_opened()) {
              const ObMacroBlockDesc *macro_desc = nullptr;
              bool need_merge = false;
              if (OB_FAIL(iter->get_curr_macro_block(macro_desc))) {
                STORAGE_LOG(WARN, "Failed to get current macro block", K(ret), KPC(iter));
              } else if (OB_ISNULL(macro_desc) || OB_UNLIKELY(!macro_desc->is_valid())) {
                ret = OB_ERR_UNEXPECTED;
                STORAGE_LOG(WARN, "Invalid macro block descriptor",
                    K(ret), KP(macro_desc), KPC(iter));
              } else if (rewrite_block_cnt < need_rewrite_block_cnt
                  && merge_ctx_->need_rewrite_macro_block(*macro_desc)) {
                if (OB_FAIL(rewrite_macro_block(minimum_iters_))) {
                  STORAGE_LOG(WARN, "Failed to rewrite macro block", K(ret), KPC(merge_ctx_));
                } else {
                  rewrite = true;
                  ++rewrite_block_cnt;
                }
	            } else if (OB_FAIL(macro_writer_.check_data_macro_block_need_merge(*macro_desc, need_merge))) {
                STORAGE_LOG(WARN, "Failed to check data macro block need merge", K(ret));
              } else if (need_merge) {
                if(OB_FAIL(rewrite_macro_block(minimum_iters_))) {
                  STORAGE_LOG(WARN, "Failed to rewrite macro block", K(ret), KPC(merge_ctx_));
                } else {
                  rewrite = true;
                }
              } else if (OB_FAIL(process(*macro_desc))) {
                STORAGE_LOG(WARN, "Fail to append macro block", K(ret), KPC(iter));
              } else if (OB_NOT_NULL(merge_progress_)) {
                reuse_row_cnt += macro_desc->row_count_;
                int tmp_ret = OB_SUCCESS;
                if (OB_SUCCESS != (tmp_ret = merge_progress_->update_row_count(idx, macro_desc->row_count_))) {
                  STORAGE_LOG(WARN, "failed to update scanned row count in merge progress", K(tmp_ret), K(idx), KPC(merge_progress_));
                }
              }
            } else if (!iter->is_micro_block_opened()) {
              // only micro_merge_iter will set the micro_block_opened flag
              const ObMicroBlock *micro_block;
              if (OB_FAIL(iter->get_curr_micro_block(micro_block))) {
                STORAGE_LOG(WARN, "Failed to get current micro block", K(ret), KPC(iter));
              } else if (OB_ISNULL(micro_block)) {
                ret = OB_ERR_UNEXPECTED;
                STORAGE_LOG(WARN, "Unexpected null micro block", K(ret), KPC(iter));
              } else if (OB_FAIL(process(*micro_block))) {
                STORAGE_LOG(WARN, "Failed to append micro block", K(ret), K(micro_block));
              } else if (OB_NOT_NULL(merge_progress_)) {
                int64_t iter_scanned_row_cnt = 0;
                reuse_row_cnt += micro_block->header_.row_count_;
                int tmp_ret = OB_SUCCESS;
                if (OB_SUCCESS != (tmp_ret = merge_progress_->update_row_count(idx, micro_block->header_.row_count_))) {
                  STORAGE_LOG(WARN, "failed to update scanned row count in merge progress", K(tmp_ret), K(idx), KPC(merge_progress_));
                }
              }
            } else {
              ret = OB_ERR_UNEXPECTED;
              STORAGE_LOG(WARN, "cur row is null, but block opened", K(ret), KPC(iter));
            }
            if (OB_FAIL(ret) || rewrite) {
              // rewrite macro block has already next macro block
            } else if (OB_FAIL(iter->next())) {
              if (OB_ITER_END == ret) {
                ret = OB_SUCCESS;
              } else {
                STORAGE_LOG(WARN, "Failed to get next row", K(ret));
              }
            }
          } else {
            if (OB_FAIL(partition_fuser_->fuse_row(minimum_iters_))) {
              STORAGE_LOG(WARN, "Failed to fuse row", KPC_(partition_fuser), K(ret));
            } else if (OB_FAIL(process(*partition_fuser_->get_result_row()))) {
              STORAGE_LOG(WARN, "Failed to process row", K(ret), K(*partition_fuser_->get_result_row()));
              if (GCONF._enable_compaction_diagnose) {
                ObPartitionMergeDumper::print_error_info(ret, *merge_ctx_, merge_iters);
              }
            } else if (OB_FAIL(partition_fuser_->calc_column_checksum(false))) {
              STORAGE_LOG(WARN, "Failed to calculate column checksum", K(ret));
            } else if (OB_FAIL(move_iters_next(minimum_iters_))) {
              STORAGE_LOG(WARN, "Failed to move merge iters next", K(ret));
            }

          }

          // updating merge progress should not have effect on normal merge process
          if (REACH_TENANT_TIME_INTERVAL(ObPartitionMergeProgress::UPDATE_INTERVAL)) {
            if (OB_NOT_NULL(merge_progress_) && (OB_SUCC(ret) || ret == OB_ITER_END)) {
              int tmp_ret = OB_SUCCESS;
              int64_t iter_row_count = reuse_row_cnt;
              for (int64_t i = 0; i < merge_iters.count(); ++i) {
                iter_row_count += merge_iters.at(i)->get_iter_row_count();
              }
              if (OB_SUCCESS != (tmp_ret = merge_progress_->update_merge_progress(idx, iter_row_count, macro_writer_.get_macro_block_write_ctx().get_macro_block_count()))) {
                STORAGE_LOG(WARN, "failed to update merge progress", K(tmp_ret));
              }
            }
          }
        }
      } // end of while
    }
    if (OB_ITER_END == ret) {
      if (OB_FAIL(end_merge_partition(merge_iters))) {
        STORAGE_LOG(WARN, "Fail to close partition merger", K(ret));
      }
    }
  }
  clean_iters_and_reset(merge_iters);

  return ret;
}

int ObPartitionMajorMerger::check_row_iters_purge(MERGE_ITER_ARRAY &minimum_iters,
                                                  ObPartitionMergeIter *base_major_iter,
                                                  bool &can_purged)
{
  int ret = OB_SUCCESS;
  const ObDatumRow *curr_row = nullptr;
  can_purged = false;

  if (OB_ISNULL(base_major_iter) || minimum_iters.empty()) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid arguments to check rowiters purge", K(ret), KP(base_major_iter), K(minimum_iters));
  } else if (!base_major_iter->is_base_iter()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (merge_ctx_->is_full_merge_) {
    // skip purte checking
  } else if (OB_ISNULL(curr_row = minimum_iters.at(0)->get_curr_row())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("cur row of minor sstable row iter is unexpected null", K(ret), KPC(minimum_iters.at(0)));
  } else if (curr_row->row_flag_.is_delete())  {
    // we cannot trust the dml flag
    // https://work.aone.alibaba-inc.com/issue/24123732

    // TODO we need add first dml to optimize the purge cost
    //const ObDatumRow *tmp_row = nullptr;
    //if (OB_ISNULL(tmp_row = minimum_iters.at(minimum_iters.count() - 1)->get_curr_row())) { // get the oldest iter
      //ret = OB_ERR_UNEXPECTED;
      //LOG_WARN("cur row of minor sstable row iter is unexpected null", K(ret), K(minimum_iters));
    //} else if (tmp_row->row_flag_.is_insert() || tmp_row->row_flag_.is_insert_delete()) {
      //// contain insert row, no need to check exist in major_sstable
      //can_purged = true;
      //LOG_DEBUG("merge check_row_iters_purge", K(can_purged), "flag", tmp_row->row_flag_);
    //} else {
      bool is_exist = false;
      if (OB_FAIL(base_major_iter->exist(curr_row, is_exist))) {
        STORAGE_LOG(WARN, "Failed to check if rowkey exist in base iter", K(ret));
      } else if (!is_exist) {
        can_purged = true;
        LOG_DEBUG("merge check_row_iters_purge", K(can_purged), K(*curr_row));
      }
    //}
  }

  return ret;
}

int ObPartitionMajorMerger::find_minimum_iters(const MERGE_ITER_ARRAY &macro_row_iters,
                                               MERGE_ITER_ARRAY &minimum_iters)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObMajorPartitionMergeFuser is not inited", K_(is_inited), K(ret));
  } else if (OB_UNLIKELY(macro_row_iters.empty())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "The macro row iters array is empty", K(macro_row_iters.count()), K(ret));
  } else {
    ObPartitionMergeIter *macro_row_iter = nullptr;
    bool is_purged = true;
    int cmp_ret = 0;
    minimum_iters.reuse();
    while (OB_SUCC(ret) && is_purged) {
      is_purged = false;
      for (int64_t i = macro_row_iters.count() - 1; OB_SUCC(ret) && i >= 0; --i) {
        if (OB_ISNULL(macro_row_iter = macro_row_iters.at(i))) {
          ret = OB_ERR_UNEXPECTED;
          STORAGE_LOG(WARN, "Unexpected null macro iter", K(ret));
        } else if (macro_row_iter->is_iter_end()) {
          // skip end macro row iter
        } else if (minimum_iters.empty()) {
          if (OB_FAIL(minimum_iters.push_back(macro_row_iter))) {
            STORAGE_LOG(WARN, "Fail to push macro iter to minimum iters", K(ret));
          }
        } else if (OB_FAIL(compare_row_iters_simple(minimum_iters.at(0), macro_row_iter, cmp_ret))) {
          STORAGE_LOG(WARN, "Failed to compare row iters", K(ret));
          // if macro_need_open == true, the macro_row_iter must be the base major iter,
          // all the minor sstable iters are using row iterator
        } else if (macro_need_open(cmp_ret)
            && OB_FAIL(check_row_iters_purge(minimum_iters, macro_row_iter, is_purged))) {
          STORAGE_LOG(WARN, "Failed to check purge row iters", K(ret));
        } else if (is_purged) {
          if (OB_FAIL(move_iters_next(minimum_iters))) {
            STORAGE_LOG(WARN, "Failed to next minium row iters", K(ret));
          } else {
            STORAGE_LOG(INFO, "Macro row iters is purged", K(ret));
            minimum_iters.reuse();
            break; // end for
          }
        } else if (macro_need_open(cmp_ret)
            && OB_FAIL(compare_row_iters_range(minimum_iters.at(0), macro_row_iter, cmp_ret))) {
          STORAGE_LOG(WARN, "Failed to compare row iters", K(ret));
        } else if (OB_UNLIKELY(macro_need_open(cmp_ret))) {
          ret = OB_ERR_UNEXPECTED;
          STORAGE_LOG(ERROR, "Unexpected compare result", K(cmp_ret), K(ret));
        } else {
          if (cmp_ret < 0) {
            minimum_iters.reuse();
          }
          if (cmp_ret <= 0) {
            if (!macro_row_iter->is_iter_end() && OB_FAIL(minimum_iters.push_back(macro_row_iter))) {
              STORAGE_LOG(WARN, "Fail to push macro_row_iter to minimum_iters", K(ret));
            }
          }
        }
      } // end of for
    } // end of while

  }

  return ret;
}

int ObPartitionMajorMerger::get_macro_block_count_to_rewrite(const ObDatumRange &merge_range,
                                                             int64_t &need_rewrite_block_cnt)
{
  int ret = OB_SUCCESS;

  need_rewrite_block_cnt = 0;
  if (merge_ctx_->is_full_merge_ || merge_ctx_->tables_handle_.get_count() == 0) {
    // minor merge and full merge no need to calculate rewrite block cnt
  } else if (!merge_ctx_->tables_handle_.get_tables().at(0)->is_sstable()) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(ERROR, "Unexpected first table for major merge", K(ret), K(merge_ctx_->tables_handle_));
  } else {
    ObSSTable *first_sstable = nullptr;
    const int64_t progressive_merge_num = merge_ctx_->progressive_merge_num_;
    if (OB_ISNULL(first_sstable = static_cast<ObSSTable *>(merge_ctx_->tables_handle_.get_tables().at(0)))) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "Unexpected null first sstable", K(ret), K(merge_ctx_->tables_handle_));
    } else if (merge_ctx_->progressive_merge_step_ < progressive_merge_num) {
      ObSSTableSecMetaIterator *sec_meta_iter;
      ObDataMacroBlockMeta macro_meta;
      if (OB_FAIL(first_sstable->scan_secondary_meta(
          allocator_,
          merge_range,
          merge_ctx_->tablet_handle_.get_obj()->get_index_read_info(),
          blocksstable::DATA_BLOCK_META,
          sec_meta_iter))) {
        LOG_WARN("Fail to scan secondary meta", K(ret), K(merge_range));
      }
      while (OB_SUCC(ret)) {
        if (OB_FAIL(sec_meta_iter->get_next(macro_meta))) {
          if (OB_ITER_END != ret) {
            STORAGE_LOG(WARN, "Failed to get next macro block", K(ret));
          } else {
            ret = OB_SUCCESS;
            break;
          }
        } else if (macro_meta.val_.progressive_merge_round_ < merge_ctx_->progressive_merge_round_) {
          ++need_rewrite_block_cnt;
        }
      }
      if (OB_NOT_NULL(sec_meta_iter)) {
        sec_meta_iter->~ObSSTableSecMetaIterator();
        allocator_.free(sec_meta_iter);
      }
      if (OB_SUCC(ret)) {
        need_rewrite_block_cnt = std::max(need_rewrite_block_cnt / (progressive_merge_num -
                                                                    merge_ctx_->progressive_merge_step_), 1L);
        STORAGE_LOG(INFO, "There are some macro block need rewrite", "tablet_id", merge_ctx_->param_.tablet_id_,
                    K(need_rewrite_block_cnt), K(merge_ctx_->progressive_merge_step_),
                    K(merge_ctx_->progressive_merge_num_), K(merge_ctx_->progressive_merge_round_));
      }
    }
  }
  return ret;
}

int ObPartitionMajorMerger::rewrite_macro_block(MERGE_ITER_ARRAY &minimum_iters)
{
  int ret = OB_SUCCESS;
  ObPartitionMergeIter *iter = static_cast<ObPartitionMergeIter *>(minimum_iters.at(0));
  if (minimum_iters.count() != 1) {
    ret = OB_INNER_STAT_ERROR;
    STORAGE_LOG(WARN, "Unexpected minimum iters to rewrite macro block", K(ret), K(minimum_iters));
  } else if (!partition_fuser_->is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected partition fuser", KPC(partition_fuser_), K(ret));
  } else if (OB_FAIL(iter->open_curr_range(true /* rewrite */))) {
    STORAGE_LOG(WARN, "Failed to open the curr macro block", K(ret));
  } else {
    STORAGE_LOG(DEBUG, "Rewrite macro block", KPC(iter));
    // TODO maybe we need use macro_block_ctx to decide wheather the result row came from the same macro block
    while (OB_SUCC(ret) && iter->is_macro_block_opened()) {
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(partition_fuser_->fuse_row(minimum_iters))) {
        STORAGE_LOG(WARN, "Failed to fuse row", K(ret));
      } else if (OB_FAIL(process(*partition_fuser_->get_result_row()))) {
        STORAGE_LOG(WARN, "Failed to process row", K(ret));
      } else if (OB_FAIL(partition_fuser_->calc_column_checksum(true))) {
        STORAGE_LOG(WARN, "Failed to calculate column checksum", K(ret));
      } else if (OB_FAIL(iter->next())) {
        if (OB_ITER_END != ret) {
          STORAGE_LOG(WARN, "Failed to iter next row", K(ret));
        }
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

int ObPartitionMajorMerger::check_need_reuse_base_sstable(MERGE_ITER_ARRAY &merge_iters,
                                                     int need_rewrite_count,
                                                     bool is_full_merge,
                                                     bool &is_need_reuse_sstable) const
{
  int ret = OB_SUCCESS;
  is_need_reuse_sstable = true;
  ObPartitionMergeIter *iter = nullptr;

  if (is_full_merge || need_rewrite_count != 0) {
    is_need_reuse_sstable = false;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < merge_iters.count() && is_need_reuse_sstable; ++i) {
      if (OB_ISNULL(iter = merge_iters.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected null iter", K(ret), K(merge_iters));
      } else if (!iter->is_iter_end() && iter->is_base_sstable_iter()) {
        const ObSSTable *sstable = static_cast<const ObSSTable *>(iter->get_table());
        if (sstable->is_small_sstable()) {
          is_need_reuse_sstable = false;
        }
      } else if (!iter->is_iter_end() && !iter->is_base_sstable_iter()) {
        is_need_reuse_sstable = false;
      }
    }
  }

  return ret;
}

int ObPartitionMajorMerger::reuse_base_sstable(MERGE_ITER_ARRAY &merge_iters)
{
  int ret = OB_SUCCESS;
  ObPartitionMergeIter *base_iter = nullptr;
  const ObMacroBlockDesc *macro_desc = nullptr;

  if (merge_iters.empty() || OB_ISNULL(base_iter = merge_iters.at(0)) || !base_iter->is_base_sstable_iter()) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "unexpected merge_iters", K(ret), K(merge_iters));
  } else {
    while (OB_SUCC(ret)) {
      if (base_iter->is_iter_end()) {
        ret = OB_ITER_END;
      } else if (base_iter->is_macro_block_opened()) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "unexpected macro block opened", K(ret), KPC(base_iter));
      } else if (OB_FAIL(base_iter->get_curr_macro_block(macro_desc))) {
        STORAGE_LOG(WARN, "Failed to get current macro block", K(ret), KPC(base_iter));
      } else if (OB_ISNULL(macro_desc) || OB_UNLIKELY(!macro_desc->is_valid())) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Invalid macro block descriptor", K(ret), KP(macro_desc), KPC(base_iter));
      } else if (OB_FAIL(process(*macro_desc))) {
        STORAGE_LOG(WARN, "Fail to append macro block", K(ret), KPC(base_iter));
      } else if (OB_FAIL(base_iter->next())) {
        if (OB_ITER_END != ret) {
          STORAGE_LOG(WARN, "Failed to get next", K(ret), KPC(base_iter));
        }
      }
    }
  }

  return ret;
}


/*
 *ObPartitionMinorMergerV2
 */

ObPartitionMinorMerger::ObPartitionMinorMerger()
  : rowkey_minimum_iters_(DEFAULT_ITER_ARRAY_SIZE, ModulePageAllocator(allocator_)),
    minimum_iter_idxs_(DEFAULT_ITER_ARRAY_SIZE, ModulePageAllocator(allocator_)),
    cols_id_map_(nullptr),
    bf_macro_writer_(),
    need_build_bloom_filter_(false)
{
}

ObPartitionMinorMerger::~ObPartitionMinorMerger()
{
  reset();
}

void ObPartitionMinorMerger::reset()
{
  rowkey_minimum_iters_.reset();
  minimum_iter_idxs_.reset();
  bf_macro_writer_.reset();
  need_build_bloom_filter_ = false;
  if (nullptr != cols_id_map_) {
    cols_id_map_->~ColumnMap();
    cols_id_map_ = nullptr;
  }
  ObPartitionMerger::reset();
}

int ObPartitionMinorMerger::open(ObTabletMergeCtx &ctx, const int64_t idx)
{
  int ret = OB_SUCCESS;

  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    STORAGE_LOG(WARN, "ObPartitionMergerV2 is init twice", K(ret), K(*this));
  } else if (OB_UNLIKELY(!ctx.is_valid() || idx < 0)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to init ObPartitionMergerV2", K(ret), K(ctx), K(idx));
  } else if (OB_FAIL(init_data_store_desc(ctx))) {
    STORAGE_LOG(WARN, "Failed to init data store desc", K(ret));
  } else {
    merge_ctx_ = &ctx;
    if (OB_NOT_NULL(ctx.merge_progress_)) {
      merge_progress_ = ctx.merge_progress_;
    }
    task_idx_ = idx;
    data_store_desc_.sstable_index_builder_ = ctx.get_merge_info().get_index_builder();
    // TODO we produce all these wired codes because of the ugly data_store_desc
    // we should rewrite the ObDataStoreDesc shortly
    if (OB_FAIL(check_need_prebuild_bloomfilter())) {
      STORAGE_LOG(WARN, "Failed to check need prebuild bloomfilter", K(ret));
    } else {
      is_inited_ = true;
    }
  }

  return ret;
}

int ObPartitionMinorMerger::check_need_prebuild_bloomfilter()
{
  int ret = OB_SUCCESS;

  if (merge_ctx_->parallel_merge_ctx_.get_concurrent_cnt() != 1) {
    data_store_desc_.need_prebuild_bloomfilter_ = false;
  } else if (MTL_ID() < OB_MAX_RESERVED_TENANT_ID) {
    // only check user table
    data_store_desc_.need_prebuild_bloomfilter_ = false;
  } else if (data_store_desc_.need_prebuild_bloomfilter_) {
    ObLS *ls = nullptr;
    common::ObRole curr_ls_role;
    int64_t proposal_id = OB_INVALID_TIMESTAMP;
    int64_t optimal_prefix = 0;
    if (OB_ISNULL(ls = merge_ctx_->ls_handle_.get_ls())) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "Failed to get ls from merge ctx", K(ret));
    } else if (OB_FAIL(MTL(logservice::ObLogService*)->get_palf_role(
        ls->get_ls_id(), curr_ls_role, proposal_id))) {
      STORAGE_LOG(WARN, "Get role failed", K(ret), KPC_(merge_ctx));
      data_store_desc_.need_prebuild_bloomfilter_ = false;
    } else if (!common::is_strong_leader(curr_ls_role)) {
      data_store_desc_.need_prebuild_bloomfilter_ = false;
    } else if (OB_FAIL(ls->get_tablet_svr()->get_bf_optimal_prefix(optimal_prefix))) {
      STORAGE_LOG(WARN, "Failed to get optimal prefix", K(ret));
    } else if (optimal_prefix <= 0 || optimal_prefix > data_store_desc_.schema_rowkey_col_cnt_) {
      data_store_desc_.need_prebuild_bloomfilter_ = false;
    } else {
      data_store_desc_.bloomfilter_rowkey_prefix_ = optimal_prefix;
      if (OB_SUCCESS != init_bloomfilter_writer()) {
        STORAGE_LOG(WARN, "Failed to init bloom filter writer", K(ret));
      }
    }
  }

  return ret;
}

int ObPartitionMinorMerger::init_bloomfilter_writer()
{
  int ret = OB_SUCCESS;

//  if (OB_FAIL(bf_macro_writer_.init(data_store_desc_))) {
//    STORAGE_LOG(WARN, "Failed to init bloomfilter macro writer", K(ret));
//  } else {
//    ObBloomFilterDataReader bf_macro_reader;
//    ObBloomFilterCacheValue bf_cache_value;
//    ObITable *table = nullptr;
//    ObSSTable *sstable = nullptr;
//    need_build_bloom_filter_ = true;
//    for (int64_t i = 0; OB_SUCC(ret) && need_build_bloom_filter_
//         && i < merge_ctx_->tables_handle_.get_tables().count(); i++) {
//      if (OB_ISNULL(table = merge_ctx_->tables_handle_.get_tables().at(i))) {
//        ret = OB_ERR_UNEXPECTED;
//        STORAGE_LOG(WARN, "Unexpected null table", KP(table), K(ret));
//      } else if (!table->is_sstable()) {
//        break;
//      } else if (FALSE_IT(sstable = reinterpret_cast<ObSSTable *>(table))) {
//      } else if (0 == sstable->get_meta().get_basic_meta().row_count_) {
//        // skip empty sstable
//      } else if (!sstable->has_bloom_filter_macro_block()) {
//        need_build_bloom_filter_ = false;
//      } else  if (OB_FAIL(bf_macro_reader.read_bloom_filter(
//          sstable->get_meta().get_macro_info().get_bf_block_id(), bf_cache_value))) {
//        if (OB_NOT_SUPPORTED != ret) {
//          STORAGE_LOG(WARN, "Failed to read bloomfilter cache", K(ret));
//        }
//      } else if (OB_UNLIKELY(!bf_cache_value.is_valid())) {
//        ret = OB_ERR_UNEXPECTED;
//        STORAGE_LOG(WARN, "Unexpected bloomfilter cache value", K(bf_cache_value), K(ret));
//      } else if (OB_FAIL(bf_macro_writer_.append(bf_cache_value))) {
//        if (OB_NOT_SUPPORTED != ret) {
//          STORAGE_LOG(WARN, "Failed to append bloomfilter cache value", K(ret));
//        }
//      }
//    }
//  }
//  if (OB_FAIL(ret) || !need_build_bloom_filter_) {
//    if (OB_NOT_SUPPORTED == ret) {
//      ret = OB_SUCCESS;
//    }
//    need_build_bloom_filter_ = false;
//    bf_macro_writer_.reset();
//  }

  return ret;
}

int ObPartitionMinorMerger::close()
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(ObPartitionMerger::close())) {
    STORAGE_LOG(WARN, "Failed to finish merge for partition merger", K(ret));
  } else if (need_build_bloom_filter_ && bf_macro_writer_.get_row_count() > 0) {
    if (OB_FAIL(bf_macro_writer_.flush_bloom_filter())) {
      STORAGE_LOG(WARN, "Failed to flush bloomfilter macro block", K(ret));
    } else if (OB_UNLIKELY(bf_macro_writer_.get_block_write_ctx().is_empty())) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "Unexpected macro block write ctx", K(ret));
    } else if (OB_FAIL(ObStorageCacheSuite::get_instance().get_bf_cache().put_bloom_filter(
                           MTL_ID(),
                           bf_macro_writer_.get_block_write_ctx().macro_block_list_.at(0),
                           bf_macro_writer_.get_bloomfilter_cache_value()))) {
      if (OB_ENTRY_EXIST != ret) {
        STORAGE_LOG(WARN, "Fail to put value to bloom_filter_cache", K(data_store_desc_.tablet_id_), K(ret));
      }
      ret = OB_SUCCESS;
    } else {
      STORAGE_LOG(INFO, "Succ to put value to bloom_filter_cache", K(data_store_desc_.tablet_id_));
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(merge_ctx_->get_merge_info().add_bloom_filter(bf_macro_writer_.get_block_write_ctx()))) {
        STORAGE_LOG(WARN, "Failed to add bloomfilter block ctx to merge context", K(ret));
      }
    }
  }

  return ret;
}

int ObPartitionMinorMerger::append_bloom_filter(const ObDatumRow &row)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!row.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "The row is invalid to append", K(row), K(ret));
  } else if (OB_UNLIKELY(!need_build_bloom_filter_)) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected status for append bloomfilter", K_(need_build_bloom_filter), K(ret));
  } else {
    ObDatumRowkey rowkey;
    if (OB_FAIL(rowkey.assign(row.storage_datums_, data_store_desc_.schema_rowkey_col_cnt_))) {
      STORAGE_LOG(WARN, "Failed to assign datum rowkey", K(ret), K(row), K_(data_store_desc));
    } else if (OB_FAIL(bf_macro_writer_.append(rowkey, data_store_desc_.datum_utils_))) {
      if (OB_NOT_SUPPORTED == ret) {
        ret = OB_SUCCESS;
        need_build_bloom_filter_ = false;
        bf_macro_writer_.reset();
      } else {
        STORAGE_LOG(WARN, "Failed to append row to bloomfilter", K(row), K(ret));
      }
    }
  }

  return ret;
}


int ObPartitionMinorMerger::inner_process(const ObDatumRow &row)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(macro_writer_.append_row(row))) {
    STORAGE_LOG(WARN, "Failed to append row to macro writer", K(ret));
  } else if (need_build_bloom_filter_ && OB_FAIL(append_bloom_filter(row))) {
    STORAGE_LOG(WARN, "Failed to append row to bloomfilter", K(ret));
  } else {
    STORAGE_LOG(DEBUG, "Success to append row to minor macro writer", K(ret), K(row));
  }

  return ret;
}

int ObPartitionMinorMerger::init_partition_fuser(const ObMergeParameter &merge_param)
{
  int ret = OB_SUCCESS;
  partition_fuser_ = nullptr;
  if (OB_ISNULL(partition_fuser_ = alloc_merge_helper<ObFlatMinorPartitionMergeFuser>())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    STORAGE_LOG(WARN, "Failed to allocate memory for partition fuser", K(ret), K(merge_param));
  } else if (OB_FAIL(partition_fuser_->init(merge_param))) {
    STORAGE_LOG(WARN, "Failed to init partition fuser", K(ret));
  }

  if (OB_FAIL(ret)) {
    if (OB_NOT_NULL(partition_fuser_)) {
      partition_fuser_->~ObIPartitionMergeFuser();
      partition_fuser_ = nullptr;
    }
  }

  return ret;
}


int ObPartitionMinorMerger::init_merge_iters(ObIPartitionMergeFuser &fuser,
                                             ObMergeParameter &merge_param,
                                             MERGE_ITER_ARRAY &merge_iters)
{
  int ret = OB_SUCCESS;
  ObPartitionMergeIter *merge_iter = nullptr;
  merge_iters.reset();

  ObITable *table = nullptr;
  for (int64_t i = 0; OB_SUCC(ret) && i < merge_param.tables_handle_->get_count(); i++) {
    if (OB_ISNULL(table = merge_param.tables_handle_->get_table(i))) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "Unexpected null table", K(ret), K(i), K(merge_param));
    } else if (OB_UNLIKELY(table->is_remote_logical_minor_sstable())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected remote minor sstable", K(ret), KP(table));
    } else if (table->is_sstable() && static_cast<ObSSTable *>(table)->get_meta().get_basic_meta().get_data_macro_block_count() <= 0) {
      // do nothing. don't need to construct iter for empty sstable
      FLOG_INFO("table is empty, need not create iter", K(i), KPC(table));
      continue;
    } else if (storage::is_backfill_tx_merge(merge_param.merge_type_)) {
      merge_iter = alloc_merge_helper<ObPartitionMinorRowMergeIter> ();
    } else if (merge_param.is_multi_version_minor_merge()) {
      if (!merge_param.is_mini_merge() && 0 == i && !merge_param.is_full_merge_ &&
          !(static_cast<ObSSTable *>(table))->is_small_sstable()) {
        merge_iter = alloc_merge_helper<ObPartitionMinorMacroMergeIter>();
      } else {
        merge_iter = alloc_merge_helper<ObPartitionMinorRowMergeIter> ();
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_ISNULL(merge_iter)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        STORAGE_LOG(WARN, "Failed to alloc memory for merge iter", K(ret));
      } else if (OB_FAIL(merge_iter->init(merge_param,
                                          fuser.get_multi_version_column_ids(),
                                          data_store_desc_.row_store_type_, i))) {
        if (OB_UNLIKELY(OB_BEYOND_THE_RANGE != ret)) {
          STORAGE_LOG(WARN, "Failed to init merge iter", K(ret), K(i));
        } else {
          ObITable *table = merge_param.tables_handle_->get_table(i);
          STORAGE_LOG(WARN, "Ignore the sstable which beyond the range", K(i), K(merge_param.merge_range_), KPC(table));
          merge_iter->~ObPartitionMergeIter();
          merge_iter = nullptr;
          ret = OB_SUCCESS;
        }
      } else if (OB_FAIL(merge_iters.push_back(merge_iter))) {
        STORAGE_LOG(WARN, "Failed to push back merge iter", K(ret), KPC(merge_iter));
      } else {
        STORAGE_LOG(INFO, "Succ to init iter", K(ret), K(i), KPC(merge_iter), K(fuser.get_multi_version_column_ids()));
        merge_iter = nullptr;
      }
    }
  }

  if (OB_FAIL(ret)) {
    if (nullptr != merge_iter) {
      merge_iter->~ObPartitionMergeIter();
      merge_iter = nullptr;
    }
  }

  return ret;
}

int ObPartitionMinorMerger::merge_partition(ObTabletMergeCtx &ctx, const int64_t idx)
{
  int ret = OB_SUCCESS;
  MERGE_ITER_ARRAY merge_iters;
  ObMergeParameter merge_param;

  if (OB_FAIL(open(ctx, idx))) {
    STORAGE_LOG(WARN, "Failed to open partition minor merge fuse", K(ret));
  } else if (OB_FAIL(prepare_merge_partition(merge_param, merge_iters))) {
    STORAGE_LOG(WARN, "Failed to prepare merge partition", K(ret));
  } else {
    if (merge_iters.empty()) {
      ret = OB_ITER_END;
    } else if (OB_FAIL(move_iters_next(merge_iters))) {
      STORAGE_LOG(WARN, "Failed to move merge iters next", K(ret));
    } else {
      int64_t reuse_row_cnt = 0;
      while (OB_SUCC(ret)) {
        share::dag_yield();
        //find minimum merge iter
        if (OB_FAIL(find_rowkey_minimum_iters(merge_iters, rowkey_minimum_iters_))) {
          STORAGE_LOG(WARN, "Failed to find minimum iters", K(ret));
        } else if (rowkey_minimum_iters_.empty()) {
          ret = OB_ITER_END;
        } else if (1 == rowkey_minimum_iters_.count()
            && nullptr == rowkey_minimum_iters_.at(0)->get_curr_row()) {
          // only one iter, output its' macro block
          ObPartitionMergeIter *iter = rowkey_minimum_iters_.at(0);
          bool need_merge = false;
          if (iter->is_macro_block_opened()) {
            ret = OB_ERR_UNEXPECTED;
            STORAGE_LOG(WARN, "cur row is null, but block is opened", K(ret), KPC(iter));
          } else {
            const ObMacroBlockDesc *macro_desc = nullptr;
            if (OB_FAIL(iter->get_curr_macro_block(macro_desc))) {
              STORAGE_LOG(WARN, "Failed to get current micro block", K(ret), KPC(iter));
            } else if (OB_ISNULL(macro_desc)) {
              ret = OB_ERR_UNEXPECTED;
              STORAGE_LOG(WARN, "Unexpected null macro block", K(ret), KP(macro_desc), KPC(iter));
            } else if (OB_FAIL(macro_writer_.check_data_macro_block_need_merge(*macro_desc, need_merge))) {
              STORAGE_LOG(WARN, "Failed to check data macro block need merge", K(ret));
            } else if (need_merge) {
              if(OB_FAIL(iter->open_curr_range(true))) {
                STORAGE_LOG(WARN, "Failed to open_curr_range", K(ret));
              }
            } else if (OB_FAIL(process(*macro_desc))) {
              STORAGE_LOG(WARN, "Failed to append macro block", K(ret));
            } else if (OB_FAIL(iter->next())) {
              if (OB_ITER_END == ret) {
                ret = OB_SUCCESS;
              } else {
                STORAGE_LOG(WARN, "Failed to get next row", K(ret));
              }
            }
            if (OB_SUCC(ret)) {
              if (OB_NOT_NULL(merge_progress_) && !need_merge) {
                reuse_row_cnt += macro_desc->row_count_;
                int tmp_ret = OB_SUCCESS;
                if (OB_SUCCESS != (tmp_ret = merge_progress_->update_row_count(idx, macro_desc->row_count_))) {
                  STORAGE_LOG(WARN, "failed to update scanned row count in merge progress", K(tmp_ret), K(idx), KPC(merge_progress_));
                }
              }
            }
          }
        } else if (OB_FAIL(merge_same_rowkey_iters(rowkey_minimum_iters_))) {
          STORAGE_LOG(WARN, "Failed to merge iters with same rowkey", K(ret), K(merge_iters));
          if (GCONF._enable_compaction_diagnose) {
            ObPartitionMergeDumper::print_error_info(ret, *merge_ctx_, merge_iters);
            macro_writer_.dump_block_and_writer_buffer();
          }
        }

        // updating merge progress should not have effect on normal merge process
        if (REACH_TENANT_TIME_INTERVAL(ObPartitionMergeProgress::UPDATE_INTERVAL)) {
          if (OB_NOT_NULL(merge_progress_) && (OB_SUCC(ret) || ret == OB_ITER_END)) {
            int tmp_ret = OB_SUCCESS;
            int64_t iter_row_count = reuse_row_cnt;
            for (int64_t i = 0; i < merge_iters.count(); ++i) {
              iter_row_count += merge_iters.at(i)->get_iter_row_count();
            }
            if (OB_SUCCESS != (tmp_ret = merge_progress_->update_merge_progress(idx, iter_row_count, macro_writer_.get_macro_block_write_ctx().get_macro_block_count()))) {
              STORAGE_LOG(WARN, "failed to update merge progress", K(tmp_ret));
            }
          }
        }
      } // end of while
    }
    if (OB_ITER_END == ret) {
      if (OB_FAIL(end_merge_partition(merge_iters))) {
        STORAGE_LOG(WARN, "Fail to close partition merger", K(ret));
        if (GCONF._enable_compaction_diagnose) {
          ObPartitionMergeDumper::print_error_info(ret, *merge_ctx_, merge_iters);
          macro_writer_.dump_block_and_writer_buffer();
        }
      }
    }
  }
  clean_iters_and_reset(merge_iters);

  return ret;
}

int ObPartitionMinorMerger::merge_single_iter(ObPartitionMergeIter &merge_iter)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(merge_iter.get_curr_row())) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected empty row of merge iter", K(ret), K(merge_iter));
  } else {
    const ObDatumRow *cur_row = nullptr;
    bool finish = false;
    bool rowkey_first_row = !merge_iter.is_rowkey_first_row_already_output();
    bool shadow_already_output = merge_iter.is_rowkey_shadow_row_already_output();
    while (OB_SUCC(ret) && !finish) {
      if (OB_ISNULL(cur_row = merge_iter.get_curr_row())) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected empty row of merge iter", K(ret), K(merge_iter));
      } else if (rowkey_first_row && cur_row->is_ghost_row()) {
        // discard ghost row
        finish = true;
      } else if (rowkey_first_row) {
        const_cast<ObDatumRow *>(cur_row)->mvcc_row_flag_.set_first_multi_version_row(true);
        rowkey_first_row = false;
      } else {
        const_cast<ObDatumRow *>(cur_row)->mvcc_row_flag_.set_first_multi_version_row(false);
      }
      if (OB_FAIL(ret) || finish) {
      } else if (shadow_already_output && cur_row->is_shadow_row()) {
      } else if (OB_FAIL(process(*cur_row))) {
        STORAGE_LOG(WARN, "Failed to process row", K(ret), KPC(cur_row), K(merge_iter));
      } else if (cur_row->is_last_multi_version_row()) {
        finish = true;
      } else if (!shadow_already_output && cur_row->is_shadow_row()) {
        shadow_already_output = true;
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(merge_iter.next())) {
        if (OB_ITER_END == ret) {
          if (finish) {
            ret = OB_SUCCESS;
          } else {
            ret = OB_ERR_UNEXPECTED;
            STORAGE_LOG(ERROR, "meed iter end without Last row", K(ret), K(merge_iter), K(finish));
          }
        } else {
          STORAGE_LOG(WARN, "Fail to next merge iter", K(ret), K(merge_iter), K(finish));
        }
      } else if (!finish && OB_ISNULL(merge_iter.get_curr_row())) {
        if (OB_FAIL(merge_iter.open_curr_range(false /*for_rewrite*/))) {
          STORAGE_LOG(WARN, "Failed to open curr range", K(ret), K(merge_iter));
        }
      }
    }
  }


  return ret;
}

int ObPartitionMinorMerger::find_rowkey_minimum_iters(const MERGE_ITER_ARRAY &merge_iters,
                                                      MERGE_ITER_ARRAY &minimum_iters)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(merge_iters.empty())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "The macro row iters array is empty", K(merge_iters.count()), K(ret));
  } else {
    ObPartitionMergeIter *merge_iter = nullptr;
    int schema_rowkey_cmp_ret = 0;
    minimum_iters.reuse();
    for (int64_t i = merge_iters.count() - 1; OB_SUCC(ret) && i >= 0; --i) {
      schema_rowkey_cmp_ret = 0;
      if (OB_ISNULL(merge_iter = merge_iters.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected null macro iter", K(ret));
      } else if (merge_iter->is_iter_end()) {
        // skip end merge iter
      } else if (minimum_iters.empty()) {
        if (OB_FAIL(minimum_iters.push_back(merge_iter))) {
          STORAGE_LOG(WARN, "Fail to push macro iter to minimum iters", K(ret));
        }
      } else if (OB_FAIL(compare_row_iters_simple(minimum_iters.at(0), merge_iter, schema_rowkey_cmp_ret))) {
        STORAGE_LOG(WARN, "Failed to compare row iters", K(ret));
      } else if (macro_need_open(schema_rowkey_cmp_ret)
                 && OB_FAIL(compare_row_iters_range(minimum_iters.at(0), merge_iter, schema_rowkey_cmp_ret))) {
        STORAGE_LOG(WARN, "Failed to compare row iters", K(ret));
      } else if (OB_UNLIKELY(macro_need_open(schema_rowkey_cmp_ret))) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(ERROR, "Unexpected compare result", K(schema_rowkey_cmp_ret), K(ret));
      } else {
        if (schema_rowkey_cmp_ret < 0) {
          minimum_iters.reuse();
        }
        if (schema_rowkey_cmp_ret <= 0) {
          if (OB_FAIL(minimum_iters.push_back(merge_iter))) {
            STORAGE_LOG(WARN, "Fail to push merge_iter to minimum_iters", K(ret));
          } else {
            STORAGE_LOG(DEBUG, "Success to push merge_iter to minimum_iters", K(ret),
                K(schema_rowkey_cmp_ret), KPC(merge_iter->get_curr_row()), KPC(merge_iter));
          }
        }
      }
    } // end for
  }

  return ret;
}


int ObPartitionMinorMerger::find_minimum_iters_with_same_rowkey(MERGE_ITER_ARRAY &merge_iters,
                                                                MERGE_ITER_ARRAY &minimum_iters,
                                                                ObIArray<int64_t> &iter_idxs)
{
  int ret = OB_SUCCESS;
  ObPartitionMergeIter *base_iter = nullptr;
  ObPartitionMergeIter *merge_iter = nullptr;
  minimum_iters.reuse();
  iter_idxs.reuse();
  if (OB_UNLIKELY(merge_iters.empty())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to find minimum iters with same rowkey", K(ret),
                K(merge_iters));
  } else if (OB_ISNULL(base_iter = merge_iters.at(0))) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected null merge iter", K(ret), K(merge_iters));
  } else if (OB_FAIL(minimum_iters.push_back(base_iter))) {
    STORAGE_LOG(WARN, "Failed to push back merge iter", K(ret));
  } else if (OB_FAIL(iter_idxs.push_back(0))) {
    STORAGE_LOG(WARN, "Failed to push back iter idx", K(ret));
  } else {
    for (int64_t i = 1; OB_SUCC(ret) && i < merge_iters.count(); i++) {
      int cmp_ret = 0;
      if (OB_ISNULL(merge_iter = merge_iters.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected null merge iter", K(ret), K(i), K(merge_iters));
      } else if (OB_FAIL(merge_iter->multi_version_compare(*base_iter, cmp_ret))) {
        STORAGE_LOG(WARN, "Failed to compare multi version merge iter", K(ret));
      } else if (OB_UNLIKELY(cmp_ret < 0)) {
        minimum_iters.reuse();
        iter_idxs.reuse();
        base_iter = merge_iter;
      } else if (cmp_ret > 0) {
        // skip this merge iter
        continue;
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(minimum_iters.push_back(merge_iter))) {
        STORAGE_LOG(WARN, "Failed to push back merge iter", K(ret));
      } else if (OB_FAIL(iter_idxs.push_back(i))) {
        STORAGE_LOG(WARN, "Failed to push back iter idx", K(ret), K(i));
      }
    }
  }
  return ret;
}

int ObPartitionMinorMerger::check_first_committed_row(const MERGE_ITER_ARRAY &merge_iters)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(merge_iters.empty())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to merge iters with same rowkey", K(ret), K(merge_iters));
  } else {
    ObPartitionMergeIter *merge_iter = nullptr;
    for (int64_t i = merge_iters.count() - 1; OB_SUCC(ret) && i >= 0; i--) {
      if (OB_ISNULL(merge_iter = merge_iters.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected null merge iter", K(ret), K(merge_iters));
      } else if (merge_iter->is_compact_completed_row()) {
        // do nothing
      } else if (OB_UNLIKELY(!merge_iter->get_curr_row()->is_ghost_row())) {
        ret = OB_INNER_STAT_ERROR;
        STORAGE_LOG(WARN, "Unexpected non compact merge iter", K(ret), KPC(merge_iter));
      }
    }
  }

  return ret;
}

int ObPartitionMinorMerger::set_result_flag(MERGE_ITER_ARRAY &fuse_iters,
                                            const bool rowkey_first_row,
                                            const bool add_shadow_row,
                                            const bool need_check_last)
{
  int ret = OB_SUCCESS;
  ObPartitionMergeIter *base_iter = nullptr;
  const ObDatumRow *base_row = nullptr;

  if (OB_UNLIKELY(fuse_iters.empty())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid empty fuse iters", K(ret), K(fuse_iters));
  } else if (OB_ISNULL(base_iter = fuse_iters.at(0))) {
    ret = OB_INNER_STAT_ERROR;
    STORAGE_LOG(WARN, "Unexpected null fuse iter", K(ret), K(fuse_iters));
  } else if (OB_ISNULL(base_row = base_iter->get_curr_row())) {
    ret = OB_INNER_STAT_ERROR;
    STORAGE_LOG(WARN, "Unexpected null curr row for base iter", K(ret), KPC(base_iter));
  } else {
    const bool is_result_compact = partition_fuser_->get_result_row()->is_compacted_multi_version_row();
    ObMultiVersionRowFlag row_flag = base_row->mvcc_row_flag_;
    if (!base_row->is_uncommitted_row() && !base_row->is_ghost_row()) {
      row_flag.set_compacted_multi_version_row(is_result_compact);
    }
    if (rowkey_first_row) {
      row_flag.set_first_multi_version_row(true);
    } else {
      row_flag.set_first_multi_version_row(false);
    }
    if (base_row->is_ghost_row()) {
    } else if (!base_row->is_last_multi_version_row()) {
    } else if (!need_check_last) {
      row_flag.set_last_multi_version_row(false);
    } else {
      for (int64_t i = 1; OB_SUCC(ret) && i < fuse_iters.count(); i++) {
        if (OB_UNLIKELY(nullptr == fuse_iters.at(i) || nullptr == fuse_iters.at(i)->get_curr_row())) {
          ret = OB_INNER_STAT_ERROR;
          STORAGE_LOG(WARN, "Unexpected null fuse iter or curr row", K(ret), K(i), KPC(fuse_iters.at(i)));
        } else if (!fuse_iters.at(i)->get_curr_row()->is_last_multi_version_row()) {
          row_flag.set_last_multi_version_row(false);
          break;
        }
      }
    }
    if (OB_SUCC(ret) && add_shadow_row) {
      const ObDatumRow *result_row = partition_fuser_->get_result_row();
      if (OB_ISNULL(result_row)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Unexpected shadow row", K(ret), KPC_(partition_fuser));
      } else {
        row_flag.set_shadow_row(true);
        int64_t sql_sequence_col_idx = data_store_desc_.schema_rowkey_col_cnt_ + 1;
        result_row->storage_datums_[sql_sequence_col_idx].reuse();
        result_row->storage_datums_[sql_sequence_col_idx].set_int(-INT64_MAX);
      }
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(partition_fuser_->set_multi_version_flag(row_flag))) {
      STORAGE_LOG(WARN, "Failed to set multi version row flag and dml", K(ret));
    } else {
      STORAGE_LOG(DEBUG, "succ to set multi version row flag and dml", KPC(partition_fuser_->get_result_row()),
                  K(row_flag), KPC(base_row));
    }
  }

  return ret;
}

int ObPartitionMinorMerger::try_remove_ghost_iters(MERGE_ITER_ARRAY &merge_iters,
                                                   const bool rowkey_first_row,
                                                   MERGE_ITER_ARRAY &minimum_iters,
                                                   ObIArray<int64_t> &iter_idxs)
{
  int ret = OB_SUCCESS;
  // if new iter iters ghost row, old row may have smalled trans version
  // now we can just ignore all the ghost row since we have one normal row at least

  if (OB_UNLIKELY(merge_iters.count() < 1 || (!rowkey_first_row && merge_iters.count() == 1))) {
  } else {
    bool found_ghost = false;
    ObPartitionMergeIter *merge_iter = nullptr;
    for (int64_t i = 0; OB_SUCC(ret) && i < merge_iters.count(); i++) {
      if (OB_ISNULL(merge_iter = merge_iters.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected null merge iter", K(ret), K(i), K(merge_iters));
      } else if (merge_iter->get_curr_row()->is_ghost_row()) {
        if (!found_ghost) {
          found_ghost = true;
          minimum_iters.reuse();
          iter_idxs.reuse();
        }
        if (OB_FAIL(minimum_iters.push_back(merge_iter))) {
          STORAGE_LOG(WARN, "Failed to push back merge iter", K(ret));
        } else if (OB_FAIL(iter_idxs.push_back(i))) {
          STORAGE_LOG(WARN, "Failed to push back iter idx", K(ret), K(i));
        }
      }
    }
    if (OB_SUCC(ret) && found_ghost) {
      // not the first row, we need keep at least one ghost row for last row flag
      if (minimum_iters.count() == merge_iters.count() && !rowkey_first_row) {
      } else {
        FLOG_INFO("try to remove useless row which consists of ghost rows only",
            KPC(minimum_iters.at(0)), K(rowkey_first_row), K(iter_idxs));
        if (OB_FAIL(move_and_remove_unused_iters(merge_iters, minimum_iters, iter_idxs))) {
          STORAGE_LOG(WARN, "Failed to move and remove iters", K(ret));
        }
      }
    }
  }


  return ret;
}

int ObPartitionMinorMerger::merge_same_rowkey_iters(MERGE_ITER_ARRAY &merge_iters)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(merge_iters.empty())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to merge iters with same rowkey", K(ret), K(merge_iters));
  } else if (OB_LIKELY(merge_iters.count() == 1)) {
    if (OB_FAIL(merge_single_iter(*merge_iters.at(0)))) {
      STORAGE_LOG(WARN, "Failed to merge single merge iter", K(ret));
    }
  } else {
    bool rowkey_first_row = true;
    bool shadow_already_output = false;
    ObPartitionMergeIter *base_iter = nullptr;
    // base iter always iters the row with newer version
    while (OB_SUCC(ret) && !merge_iters.empty()) {
      bool add_shadow_row = false;
      MERGE_ITER_ARRAY *fuse_iters = &minimum_iters_;
      if (OB_FAIL(try_remove_ghost_iters(merge_iters, rowkey_first_row, minimum_iters_, minimum_iter_idxs_))) {
        STORAGE_LOG(WARN, "Failed to check and remove ghost iters", K(ret));
      } else if (OB_UNLIKELY(merge_iters.empty())) {
        // all the iters are ghost row iter
        break;
      } else if (OB_FAIL(find_minimum_iters_with_same_rowkey(merge_iters, minimum_iters_, minimum_iter_idxs_))) {
        STORAGE_LOG(WARN, "Failed to find minimum iters with same rowkey", K(ret));
      } else if (OB_UNLIKELY(minimum_iters_.empty())) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected empty minimum iters", K(ret), K(merge_iters));
      } else if (OB_ISNULL(base_iter = minimum_iters_.at(0))) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected null merge iter", K(ret), K_(minimum_iters));
      } else if (!shadow_already_output && base_iter->is_compact_completed_row()) {
        if (OB_FAIL(check_add_shadow_row(merge_iters,
                                         minimum_iters_.count() != merge_iters.count(),
                                         add_shadow_row))) {
          LOG_WARN("Failed to merge shadow row", K(ret), K(merge_iters));
        } else {
          fuse_iters = &merge_iters;
        }
      }

      if (OB_FAIL(ret)) {
      } else if (OB_ISNULL(base_iter)) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected null base iter", K(ret), K(merge_iters));
      } else if (shadow_already_output && base_iter->get_curr_row()->is_shadow_row()) {
        if (OB_UNLIKELY(1 != minimum_iters_.count())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Unexpected minimum shadow row iters", K(ret), K(minimum_iters_));
        }
      } else if (OB_FAIL(partition_fuser_->fuse_row(*fuse_iters))) {
        STORAGE_LOG(WARN, "Failed to fuse rowkey minimum iters", K(ret), KPC(fuse_iters));
      } else if (OB_FAIL(set_result_flag(*fuse_iters, rowkey_first_row, add_shadow_row,
                                         minimum_iters_.count() == merge_iters.count()))) {
        STORAGE_LOG(WARN, "Failed to calc multi version row flag", K(ret), K(add_shadow_row),
                    K(shadow_already_output), KPC(fuse_iters));
      } else if (OB_FAIL(process(*partition_fuser_->get_result_row()))) {
        STORAGE_LOG(WARN, "Failed to process row", K(ret), KPC(partition_fuser_->get_result_row()), KPC(fuse_iters));
      } else if (!shadow_already_output && base_iter->is_compact_completed_row()) {
        shadow_already_output = true;
      }

      if (OB_SUCC(ret)) {
        rowkey_first_row = false;
        if (add_shadow_row) {
          if (OB_FAIL(skip_shadow_row(*fuse_iters))) {
            LOG_WARN("Failed to skip shadow row", K(ret), K(merge_iters));
          }
        } else if (OB_FAIL(move_and_remove_unused_iters(merge_iters, minimum_iters_, minimum_iter_idxs_))) {
          LOG_WARN("Failed to move and remove iters", K(ret));
        }
      }
    }
    if (OB_FAIL(ret) && GCONF._enable_compaction_diagnose) {
      ObPartitionMergeDumper::print_error_info(ret, *merge_ctx_, merge_iters);
      macro_writer_.dump_block_and_writer_buffer();
    }
  }

  return ret;
}

int ObPartitionMinorMerger::check_add_shadow_row(MERGE_ITER_ARRAY &merge_iters, const bool contain_multi_trans, bool& add_shadow_row)
{
  int ret = OB_SUCCESS;
  add_shadow_row = false;
  if (OB_FAIL(check_first_committed_row(merge_iters))) {
    LOG_WARN("Failed to check compact first multi version row", K(ret));
  } else {
    if (contain_multi_trans) {
      add_shadow_row = true;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < merge_iters.count(); i++) {
        if (OB_UNLIKELY(nullptr == merge_iters.at(i) || nullptr == merge_iters.at(i)->get_curr_row())) {
          ret = OB_INNER_STAT_ERROR;
          LOG_WARN("Unexpected null fuse iter or curr row", K(ret), K(i), KPC(merge_iters.at(i)));
        } else if (merge_iters.at(i)->get_curr_row()->is_shadow_row()) {
          add_shadow_row = true;
          break;
        }
      }
    }
  }
  return ret;
}

int ObPartitionMinorMerger::move_and_remove_unused_iters(MERGE_ITER_ARRAY &merge_iters,
                                                         MERGE_ITER_ARRAY &minimum_iters,
                                                         ObIArray<int64_t> &iter_idxs)
{
  int ret = OB_SUCCESS;
  ObPartitionMergeIter *merge_iter = nullptr;

  for (int64_t i = minimum_iters.count() - 1; OB_SUCC(ret) && i >= 0; i--) {
    bool need_remove = false;
    if (OB_ISNULL(merge_iter = minimum_iters.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "Unexpected null merge iter", K(ret), K(i), K(minimum_iters));
    } else if (FALSE_IT(need_remove = (merge_iter->get_curr_row()->is_last_multi_version_row()))) {
    } else if (OB_FAIL(merge_iter->next())) {
      if (OB_ITER_END == ret && need_remove) {
        ret = OB_SUCCESS;
      } else {
        STORAGE_LOG(WARN, "Failed to next merge iter", K(ret), KPC(merge_iter));
      }
    } else if (!need_remove && nullptr == merge_iter->get_curr_row()) {
      if (OB_FAIL(merge_iter->open_curr_range(false /*for_rewrite*/))) {
        STORAGE_LOG(WARN, "Failed to open curr range", K(ret));
      }
    }
    if (OB_SUCC(ret) && need_remove) {
      if (OB_FAIL(merge_iters.remove(iter_idxs.at(i)))) {
        STORAGE_LOG(WARN, "Failed to remove merge iter", K(ret), K(i), K(iter_idxs),
                    K(merge_iters));
      }
    }
  }

  return ret;
}

int ObPartitionMinorMerger::skip_shadow_row(MERGE_ITER_ARRAY &merge_iters)
{
  int ret = OB_SUCCESS;
  ObPartitionMergeIter *merge_iter = nullptr;
  const ObDatumRow *merge_row = nullptr;
  for (int64_t i = merge_iters.count() - 1; OB_SUCC(ret) && i >= 0; i--) {
    if (OB_ISNULL(merge_iter = merge_iters.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("Unexpected null merge iter", K(ret), K(i), K(merge_iters));
    } else if (OB_ISNULL(merge_row = merge_iter->get_curr_row())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("Unexpected null curr row", K(ret), KPC(merge_iter));
    } else if (merge_row->is_shadow_row()) {
      if (OB_FAIL(merge_iter->next())) {
        LOG_WARN("Failed to next merge iter", K(ret), KPC(merge_iter));
      } else if (nullptr == merge_iter->get_curr_row()) {
        if (OB_FAIL(merge_iter->open_curr_range(false /*for_rewrite*/))) {
          LOG_WARN("Failed to open curr range", K(ret));
        }
      }
    } // else continue
  }

  return ret;
}

/*
 *ObPartitionMergeDumper
 */
int ObPartitionMergeDumper::generate_dump_table_name(const char *dir_name,
                                                     const ObITable *table,
                                                     char *file_name)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "table is null", K(ret));
  } else {
    int64_t pret = snprintf(
                       file_name, OB_MAX_FILE_NAME_LENGTH, "%s/%s.%s.%ld.%s.%d.%s.%ld.%s.%ld",
                       dir_name,
                       table->is_memtable() ? "dump_memtable" : "dump_sstable",
                       "tablet_id", table->get_key().tablet_id_.id(),
                       "table_type", table->get_key().table_type_,
                       "start_scn", table->get_start_scn().get_val_for_tx(),
                       "end_scn", table->get_end_scn().get_val_for_tx());
    if (pret < 0 || pret >= OB_MAX_FILE_NAME_LENGTH) {
      ret = OB_INVALID_ARGUMENT;
      STORAGE_LOG(WARN, "name too long", K(ret), K(pret), K(file_name));
    }
  }
  return ret;
}

lib::ObMutex ObPartitionMergeDumper::lock(common::ObLatchIds::MERGER_DUMP_LOCK);

int ObPartitionMergeDumper::check_disk_free_space(const char *dir_name)
{
  int ret = OB_SUCCESS;
  int64_t total_space = 0;
  int64_t free_space = 0;
  if (OB_FAIL(FileDirectoryUtils::get_disk_space(dir_name, total_space, free_space))) {
    STORAGE_LOG(WARN, "Failed to get disk space ", K(ret), K(dir_name));
  } else if (free_space < ObPartitionMergeDumper::DUMP_TABLE_DISK_FREE_PERCENTAGE * total_space) {
    ret = OB_SERVER_OUTOF_DISK_SPACE;
  }
  return ret;
}

int ObPartitionMergeDumper::judge_disk_free_space(const char *dir_name, ObITable *table)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "table is null", K(ret));
  } else {
    int64_t total_space = 0;
    int64_t free_space = 0;
    if (OB_FAIL(FileDirectoryUtils::get_disk_space(dir_name, total_space, free_space))) {
      STORAGE_LOG(WARN, "Failed to get disk space ", K(ret), K(dir_name));
    } else if (table->is_sstable()) {
      if (free_space
          - static_cast<ObSSTable *>(table)->get_meta().get_basic_meta().get_total_macro_block_count() *
          OB_DEFAULT_MACRO_BLOCK_SIZE
          < ObPartitionMergeDumper::DUMP_TABLE_DISK_FREE_PERCENTAGE * total_space) {
        ret = OB_SERVER_OUTOF_DISK_SPACE;
        STORAGE_LOG(WARN, "disk space is not enough", K(ret), K(free_space), K(total_space), KPC(table));
      }
    } else if (free_space
               - static_cast<ObMemtable *>(table)->get_occupied_size() * MEMTABLE_DUMP_SIZE_PERCENTAGE
               < ObPartitionMergeDumper::DUMP_TABLE_DISK_FREE_PERCENTAGE * total_space) {
      ret = OB_SERVER_OUTOF_DISK_SPACE;
      STORAGE_LOG(WARN, "disk space is not enough", K(ret), K(free_space), K(total_space), KPC(table));
    }
  }
  return ret;
}

bool ObPartitionMergeDumper::need_dump_table(int err_no)
{
  bool bret = false;
  if (OB_CHECKSUM_ERROR == err_no
      || OB_ERR_UNEXPECTED == err_no
      || OB_ERR_SYS == err_no
      || OB_ROWKEY_ORDER_ERROR == err_no
      || OB_ERR_PRIMARY_KEY_DUPLICATE == err_no) {
    bret = true;
  }
  return bret;
}

void ObPartitionMergeDumper::print_error_info(const int err_no,
                                              ObTabletMergeCtx &ctx,
                                              MERGE_ITER_ARRAY &merge_iters)
{
  int ret = OB_SUCCESS;
  const char *dump_table_dir = "/tmp";
  if (need_dump_table(err_no)) {
    for (int64_t midx = 0; midx < merge_iters.count(); ++midx) {
      ObPartitionMergeIter *cur_iter = merge_iters.at(midx);
      const ObMacroBlockDesc *macro_desc = nullptr;
      const ObDatumRow *curr_row = cur_iter->get_curr_row();
      if (!cur_iter->is_macro_merge_iter()) {
        if (OB_NOT_NULL(curr_row)) {
          STORAGE_LOG(WARN, "merge iter content: ", K(midx), K(cur_iter->get_table()->get_key()),
              KPC(cur_iter->get_curr_row()));
        }
      } else if (OB_FAIL(cur_iter->get_curr_macro_block(macro_desc))) {
        STORAGE_LOG(WARN, "Failed to get current micro block", K(ret), KPC(cur_iter));
      } else if (OB_ISNULL(macro_desc)) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected null macro block", K(ret), KP(macro_desc), KPC(cur_iter));
      } else if (OB_ISNULL(curr_row)) {
        STORAGE_LOG(WARN, "merge iter content: ", K(midx), K(cur_iter->get_table()->get_key()),
                    KPC(macro_desc));
      } else {
        STORAGE_LOG(WARN, "merge iter content: ", K(midx), K(cur_iter->get_table()->get_key()),
                    KPC(macro_desc), KPC(cur_iter->get_curr_row()));
      }
    }
    // dump all sstables in this merge
    ObIArray<ObITable *> &tables = ctx.tables_handle_.get_tables();
    char file_name[OB_MAX_FILE_NAME_LENGTH];
    lib::ObMutexGuard guard(ObPartitionMergeDumper::lock);
    for (int idx = 0; OB_SUCC(ret) && idx < tables.count(); ++idx) {
      ObITable *table = tables.at(idx);
      if (OB_ISNULL(table)) {
        STORAGE_LOG(WARN, "The store is NULL", K(idx), K(tables));
      } else if (OB_FAIL(compaction::ObPartitionMergeDumper::judge_disk_free_space(dump_table_dir,
                         table))) {
        if (OB_SERVER_OUTOF_DISK_SPACE != ret) {
          STORAGE_LOG(WARN, "failed to judge disk space", K(ret), K(dump_table_dir));
        }
      } else if (OB_FAIL(generate_dump_table_name(dump_table_dir, table, file_name))) {
        ret = OB_INVALID_ARGUMENT;
        STORAGE_LOG(WARN, "name too long", K(ret), K(file_name));
      } else if (table->is_sstable()) {
        if (OB_FAIL(static_cast<ObSSTable *>(table)->dump2text(dump_table_dir, *ctx.schema_ctx_.table_schema_,
                                                               file_name))) {
          if (OB_SERVER_OUTOF_DISK_SPACE != ret) {
            STORAGE_LOG(WARN, "failed to dump sstable", K(ret), K(file_name));
          }
        } else {
          STORAGE_LOG(INFO, "success to dump sstable", K(ret), K(file_name));
        }
      } else if (table->is_memtable()) {
        STORAGE_LOG(INFO, "skip dump memtable", K(ret), K(file_name));
        /*
         *if (OB_FAIL(static_cast<ObMemtable *>(table)->dump2text(file_name))) {
         *  STORAGE_LOG(WARN, "failed to dump memtable", K(ret), K(file_name));
         *}
         */
      }
    } // end for
  }
}

} //compaction
} //oceanbase
