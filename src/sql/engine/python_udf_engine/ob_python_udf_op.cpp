#define USING_LOG_PREFIX SQL_ENG

#include "ob_python_udf_op.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObPythonUDFSpec, ObOpSpec),
                    udf_exprs_);

static int max_buffer_size_ = 8192;

ObPythonUDFSpec::ObPythonUDFSpec(ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), udf_exprs_(alloc) {}

ObPythonUDFSpec::~ObPythonUDFSpec() {}

ObPythonUDFOp::ObPythonUDFOp(
    ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
  //: ObOperator(exec_ctx, spec, input), buf_exprs_(exec_ctx.get_allocator())
  : ObOperator(exec_ctx, spec, input), local_allocator_(), controller_(local_allocator_)
{
  int ret = OB_SUCCESS;
  //brs_skip_size_ = MY_SPEC.max_batch_size_;
  //predict_size_ = 256;
  //predict_size_ = MY_SPEC.max_batch_size_; //default

  max_buffer_size_ = 4096;
  // pile
  /*use_input_buf_ = false;
  use_output_buf_ = false;

  test_controller_ = true;

  if (use_input_buf_) {  
    // init input buffer
    input_buffer_.init(&local_allocator_, MY_SPEC.get_child()->output_, exec_ctx, 2 * max_buffer_size_);
  }
  if (use_output_buf_) {
    // init output buffer
    output_buffer_.init(&local_allocator_, MY_SPEC.output_, exec_ctx, 2 * max_buffer_size_);
  }*/

  const uint64_t tenant_id = ctx_.get_my_session()->get_effective_tenant_id();
  local_allocator_.set_tenant_id(tenant_id);
  local_allocator_.set_label(ObModIds::OB_SQL_EXPR);
  local_allocator_.set_ctx_id(ObCtxIds::WORK_AREA);
}

ObPythonUDFOp::~ObPythonUDFOp() {}

/* override */
/*int ObPythonUDFOp::get_next_batch(const int64_t max_row_cnt, const ObBatchRows *&batch_rows) 
{
  int ret = OB_SUCCESS;

  if (use_output_buf_) {
    while (OB_SUCC(ret) && (output_buffer_.get_size() <= output_buffer_.get_max_size() / 2) && !brs_.end_) {
      if (OB_FAIL(ObOperator::get_next_batch(max_row_cnt, batch_rows))) {
        LOG_WARN("fail to inner get next batch", K(ret));
      } else if (OB_FAIL(output_buffer_.save(eval_ctx_, brs_))){
        LOG_WARN("fail to save input batchrows", K(ret));
      }
    }
    if (!output_buffer_.is_saved()) {
      // do nothing
    } else if (OB_FAIL(output_buffer_.load(eval_ctx_, brs_, brs_skip_size_, MY_SPEC.max_batch_size_))) {
      LOG_WARN("fail to load input batchrows", K(ret));
    }
    batch_rows = &brs_;
  } else {
    ret = ObOperator::get_next_batch(max_row_cnt, batch_rows);
  }
  return ret;
}*/

int ObPythonUDFOp::inner_open()
{
  int ret = OB_SUCCESS;
  // init context
  const uint64_t tenant_id = ctx_.get_my_session()->get_effective_tenant_id();
  if (OB_FAIL(controller_.init(MY_SPEC.max_batch_size_,
                               MY_SPEC.udf_exprs_, 
                               MY_SPEC.input_exprs_))) {
    LOG_WARN("Init python udf store controller failed", K(ret));
  } else {
    // check attrs
    ret = ObOperator::inner_open();
  }
  return ret;
}

int ObPythonUDFOp::inner_close()
{
  int ret = OB_SUCCESS;
  // free attrs;
  if (OB_FAIL(controller_.free())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Free python udf store controller failed", K(ret));
  } else {
    ret = ObOperator::inner_close();
  }
  return ret;
}

int ObPythonUDFOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  // reset attrs
  const uint64_t tenant_id = ctx_.get_my_session()->get_effective_tenant_id();
  if (OB_FAIL(controller_.free()) || 
      OB_FAIL(controller_.init(MY_SPEC.max_batch_size_,
                               MY_SPEC.udf_exprs_, 
                               MY_SPEC.input_exprs_))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = ObOperator::inner_rescan();
  }
  return ret;
}

void ObPythonUDFOp::destroy()
{
  // destroy attrs
  ObOperator::destroy();
}

int ObPythonUDFOp::inner_get_next_row() 
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(child_->get_next_row())) {
    if (OB_ITER_END != ret) {
      LOG_WARN("get row from child failed", K(ret));
    }
  } else {/* do nothing */}
  return ret;
}

/*使用缓存，确保每轮迭代能够以合适的批次大小计算Python UDF
* 1. 调用Python UDF expr的子expr
* 2. 复制frame上的输入至input缓存
* 3. 重复1，2至input缓存到达阈值
* 4. 直接在input缓存上计算Python UDF，并将结果保存至output缓存
* 5. 将output缓存复制回frame上的输出
* 6. 重复5至output缓存清空
 */
/*int ObPythonUDFOp::inner_get_next_batch(const int64_t max_row_cnt)
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  const ObBatchRows *child_brs = nullptr;
  if (use_input_buf_) {
    while (OB_SUCC(ret) && (input_buffer_.get_size() <= input_buffer_.get_max_size() - MY_SPEC.max_batch_size_) && !brs_.end_) {
      if (OB_FAIL(child_->get_next_batch(max_row_cnt, child_brs))) {
        LOG_WARN("get child next batch failed", K(ret));
      } else if (brs_.copy(child_brs)) {
        LOG_WARN("copy child batch rows failed", K(ret));
      } else if (OB_FAIL(input_buffer_.save(eval_ctx_, brs_))){
        LOG_WARN("save input batchrows failed", K(ret));
      }
    }
    // 根据exprs的运行时间调整predict size
    int32_t current_size = 256; //default
    FOREACH_CNT_X(e, MY_SPEC.calc_exprs_, OB_SUCC(ret)) 
      OZ(find_predict_size((*e), current_size));
    FOREACH_CNT_X(e, MY_SPEC.output_, OB_SUCC(ret)) 
      OZ(find_predict_size((*e), current_size));
    predict_size_ = current_size;
    // 取出参数
    if (OB_FAIL(input_buffer_.load(eval_ctx_, brs_, brs_skip_size_, predict_size_))) {
      LOG_WARN("fail to load input batchrows", K(ret));
    } else if (OB_FAIL(clear_calc_exprs_evaluated_flags())) {
      LOG_WARN("fail to clear calc_exprs evaluated flages", K(ret));
    }
  } else {
    if (OB_FAIL(child_->get_next_batch(max_row_cnt, child_brs))) {
        LOG_WARN("get child next batch failed", K(ret));
    } else {
      brs_.copy(child_brs);
    }
  }
  return ret;
}*/
int ObPythonUDFOp::inner_get_next_batch(const int64_t max_row_cnt)
{
  int ret = OB_SUCCESS;
  // get data from buffer
  if (!controller_.is_output()) {
    clear_evaluated_flag();
    const ObBatchRows *child_brs = nullptr;
    while (OB_SUCC(ret) && !brs_.end_ && !controller_.is_full()) {
      if (OB_FAIL(child_->get_next_batch(max_row_cnt, child_brs))) {
        LOG_WARN("Get child next batch failed.", K(ret));
      } else if (brs_.copy(child_brs)) {
        LOG_WARN("Copy child batch rows failed.", K(ret));
      } else if (OB_FAIL(controller_.store(eval_ctx_, brs_))){
        LOG_WARN("Save input batchrows failed.", K(ret));
      }
    }
    if (OB_FAIL(ret) || OB_FAIL(controller_.process())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("Process python udf failed.", K(ret));
    }
  }
  if (OB_FAIL(ret) || OB_FAIL(controller_.restore(eval_ctx_, brs_, max_row_cnt))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Restore output batchrows failed.", K(ret));
  }
  return ret;
}


/*---private functions---*/

/* find predict_size of buffer */
int ObPythonUDFOp::find_predict_size(ObExpr *expr, int32_t &predict_size)
{
  int ret = OB_SUCCESS;
  int expr_size = 256; //default
  if (expr->type_ == T_FUN_PYTHON_UDF) {
    int size = static_cast<ObPythonUdfInfo *>(expr->extra_info_)->predict_size;
    expr_size = size > expr_size ? size : expr_size;
  } else {
    for (int32_t i = 0; i < expr->arg_cnt_; i++) {
      find_predict_size(expr->args_[i], expr_size);
    }
  }
  if (predict_size < expr_size)
    predict_size = expr_size;
  if (predict_size > max_buffer_size_)
    predict_size = max_buffer_size_;
  return ret;
}

int ObPythonUDFOp::clear_calc_exprs_evaluated_flags() 
{
  int ret = OB_SUCCESS;
  for (int i = 0; i < MY_SPEC.calc_exprs_.count(); i++) {
    MY_SPEC.calc_exprs_[i]->get_eval_info(eval_ctx_).clear_evaluated_flag();
  }
  return ret;
}

/* ------------------------------------ buffer for python_udf ----------------------------------- */
int ObVectorBuffer::init(common::ObIAllocator *alloc, const common::ObIArray<ObExpr *> &exprs, 
                         ObExecContext &exec_ctx, int64_t max_buffer_size)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(inited_)) {
    saved_size_ = 0;
    // do nothing
  } else if (exprs.count() == 0) {
    // do nothing
  } else {
    alloc_ = alloc;
    exprs_ = &exprs;
    exec_ctx_ = &exec_ctx;
    max_size_ = max_buffer_size; 
    datums_ = static_cast<ObDatum *>(exec_ctx.get_allocator().alloc(
      max_size_ * exprs.count() * sizeof(ObDatum)));
    if (OB_ISNULL(datums_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("allocate memory failed", K(ret), K(max_size_), K(exprs.count()));
    }
    inited_ = true;
    saved_size_ = 0;
  }
  return ret;
}

int ObVectorBuffer::save(ObEvalCtx &eval_ctx, ObBatchRows &brs_) 
{
  int ret = OB_SUCCESS;
  int cnt = brs_.size_ - brs_.skip_->accumulate_bit_cnt(brs_.size_);
  if (NULL == exprs_) {
    // empty expr_: do nothing
  } else if (NULL == datums_) {
    ret = OB_NOT_INIT;
  } else if (saved_size_ + cnt <= max_size_) {
    for (int64_t i = 0; i < exprs_->count(); i++) {
      ObExpr *e = exprs_->at(i);
      ObDatum *src = e->locate_batch_datums(eval_ctx);
      int64_t k = 0;
      for (int64_t j = 0; j < brs_.size_; j++) {
        /* remove skipped rows */
        if (!brs_.skip_->at(j)) {
          /* deep copy */
          datums_[i * max_size_ + saved_size_ + k].deep_copy(src[j], *alloc_);
          ++k;
        }
      }
    }
    saved_size_ += cnt;
    //brs_.size_ = 0;
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to save vector buffer", K(ret));
  }
  return ret;
}

int ObVectorBuffer::load(ObEvalCtx &eval_ctx, ObBatchRows &brs_, int64_t &brs_skip_size_ , int64_t batch_size) 
{
  int ret = OB_SUCCESS;
  if (NULL == exprs_) {
    // empty expr_: do nothing
  } else if (NULL == datums_ || saved_size_ < 0) {
    ret = OB_NOT_INIT;
  } else if (saved_size_ == 0) {
    //* brs_.size_ = 0;
    //brs_.end_ = true;
  } else {
    int64_t size = (saved_size_ < batch_size) ? saved_size_ : batch_size;
    for (int64_t i = 0; i < exprs_->count(); i++) {
      ObExpr *e = exprs_->at(i);
      /* deep copy */
      //MEMCPY(e->locate_batch_datums(eval_ctx), datums_ + i * max_size_, sizeof(ObDatum) * size);
      //* e->get_pvt_skip(eval_ctx).reset(size);
      //* e->get_evaluated_flags(eval_ctx).reset(size);

      ObDatum *src_datums = datums_ + i * max_size_;
      ObDatum *result_datums = e->locate_batch_datums(eval_ctx);
      switch(e->datum_meta_.type_) {
        case ObCharType:
        case ObVarcharType:
        case ObTinyTextType:
        case ObTextType:
        case ObMediumTextType:
        case ObLongTextType: {
          for (int j = 0; j < size; ++j) {
            e->reset_ptr_in_datum(eval_ctx, i);
            result_datums[j].set_string(src_datums[j].ptr_, src_datums[j].len_);
          }
          break;
        }
        case ObTinyIntType:
        case ObSmallIntType:
        case ObMediumIntType:
        case ObInt32Type:
        case ObIntType: {
          for (int j = 0; j < size; ++j) {
            e->reset_ptr_in_datum(eval_ctx, i);
            result_datums[j].set_int(src_datums[j].get_int());
          }
          break;
        }
        case ObDoubleType: {
          for (int j = 0; j < size; ++j) {
            e->reset_ptr_in_datum(eval_ctx, i);
            result_datums[j].set_int(src_datums[j].get_double());
          }
          break;
        }
        default: {
          //error
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("Unsupported result type.", K(ret));
        }
      }
    }
    move(size);
    /* brs_.size_ = size;
    brs_.end_ = false;
    if(size > brs_skip_size_) {
      //realloc batchrows size
      void *mem = exec_ctx_->get_allocator().alloc(ObBitVector::memory_size(size));
      if (OB_ISNULL(mem)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("allocate memory failed", K(ret));
      } else {
        brs_.skip_ = to_bit_vector(mem);
        brs_.skip_->init(size);
        brs_skip_size_ = size;
      }
    } else {
      brs_.skip_->reset(size);
    }*/
  }
  return ret;
}

int ObVectorBuffer::move(int64_t size) 
{
  int ret = OB_SUCCESS;
  if (NULL == exprs_) {
    // empty expr_: do nothing
  } else if (NULL == datums_) {
    ret = OB_NOT_INIT;
  } else if (saved_size_ > 0) {
    for (int64_t i = 0; i < exprs_->count(); i++) {
      /* shallow copy */
      MEMMOVE(datums_ + i * max_size_, datums_ + i * max_size_ + size, sizeof(ObDatum) * (saved_size_ - size)); 
    }
    saved_size_ -= size;
  }
  return ret;
}
/* ---------------------------------- ObColInputStore -------------------------------------*/
int ObColInputStore::init(const common::ObIArray<ObExpr *> &exprs,
                          int64_t batch_size,
                          int64_t length)
{
  int ret = OB_SUCCESS;
  //ObSEArray<ObExpr *, 8> exprs;
  //OZ(append(exprs, outputs));
  //OZ(append(exprs, filters));
  exprs_.init(exprs.count());
  datums_copy_.init(exprs.count());
  for (int i = 0; OB_SUCC(ret) && i < exprs.count(); ++i) {
    ObExpr *expr = exprs.at(i);
    ObDatum *datums_buf = NULL;
    if (OB_ISNULL(datums_buf = static_cast<ObDatum *>(alloc_.alloc(sizeof(ObDatum) * length)))) {
      //分配datums_buf空间
      ret = OB_INIT_FAIL;
      LOG_WARN("Memory allocation failed.", K(ret));
    } else if (OB_FAIL(exprs_.push_back(expr)) || OB_FAIL(datums_copy_.push_back(datums_buf))) {
      ret = OB_INIT_FAIL;
      LOG_WARN("Init datums_copy and other exprs failed.", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    length_ = length;
    batch_size_ = batch_size;
    saved_size_ = 0;
    output_idx_ = 0;
    inited_ = true;
  }
  return ret;
}

int ObColInputStore::free()
{
  int ret = OB_SUCCESS;
  inited_ = false;
  return ret;
}

int ObColInputStore::reuse()
{
  int ret = OB_SUCCESS;
  saved_size_ = 0;
  output_idx_ = 0;
  return ret;
}

int ObColInputStore::reset(int64_t length)
{
  int ret = OB_SUCCESS;
  for (int i = 0; i < exprs_.count(); ++i) {
    ObExpr *e = exprs_.at(i);
    ObDatum *datums_buf = static_cast<ObDatum *>(alloc_.alloc(length * sizeof(ObDatum)));
    datums_copy_.push_back(datums_buf);
  }
  length_ = length;
  saved_size_ = 0;
  output_idx_ = 0;
  return ret;
}

int ObColInputStore::save_batch(ObEvalCtx &eval_ctx, ObBatchRows &brs)
{
  int ret = OB_SUCCESS;
  int cnt = brs.size_ - brs.skip_->accumulate_bit_cnt(brs.size_);
  if (saved_size_ + cnt > length_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Other Input Store overflow.", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < exprs_.count(); ++i) {
      ObExpr *expr = exprs_.at(i);
      /*ObBitVector &my_skip = expr->get_pvt_skip(eval_ctx);
      for (int64_t j = 0; OB_SUCC(ret) && j < expr->arg_cnt_; ++j) {
        ObExpr *arg = expr->args_[j];
        if (OB_FAIL(arg->eval_batch(eval_ctx, my_skip, batch_size_))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Eval other outputs args batch result failed.", K(ret));
        }
      }*/
      ObDatum *src = expr->locate_batch_datums(eval_ctx);
      ObDatum *dst = datums_copy_.at(i);
      int j = 0, zero = 0;
      int *src_idx;
      if (expr->is_const_expr())
        src_idx = &zero;
      else
        src_idx = &j;
      int dst_idx = saved_size_; // use dst idx to reform data
      for (j = 0; OB_SUCC(ret) && j < brs.size_; ++j) {
        if (brs.skip_->at(j)) {
          /* do nothing */
        } else {
          dst[dst_idx++].deep_copy(src[*src_idx], alloc_);
        }
      }
    }
    if (OB_SUCC(ret)) {
      saved_size_ += cnt;
    }
  }
  return ret;
}

int ObColInputStore::load_batch(ObEvalCtx &eval_ctx, int64_t load_size)
{
  int ret = OB_SUCCESS;
  if (output_idx_ + load_size > saved_size_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Not enough data to load.", K(ret));
  } else {
    for (int64_t i = 0; i < exprs_.count(); ++i) {
      ObExpr *expr = exprs_.at(i);
      ObDatum *src = datums_copy_.at(i);
      ObDatum *dst = expr->locate_batch_datums(eval_ctx);
      for (int j = 0; j < load_size; ++j) {
        //expr->reset_ptr_in_datum(eval_ctx, j);
        dst[j].set_datum(src[output_idx_ + j]);
        //dst[j].deep_copy(src[output_idx_ + j], alloc_);
      }
      //expr->set_evaluated_projected(eval_ctx);
    }
    if (OB_SUCC(ret)) {
      output_idx_ += load_size;
    }
  }
  return ret;
}

/* ---------------------------------- ObPUInputStore -------------------------------- */

int ObPUInputStore::init(common::ObIAllocator *alloc, ObExpr *expr, int64_t length)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("Repeat allocation", K(ret));
  } else if (OB_ISNULL(alloc) || OB_ISNULL(expr) || length <= 0) {
    ret = OB_NOT_INIT;
    LOG_WARN("Uninit allocator and expression.", K(ret));
  } else if (expr->type_ != T_FUN_PYTHON_UDF) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("Not Python UDF Expr Type.", K(ret));
  } else {
    alloc_ = alloc;
    expr_ = expr;
    length_ = length;
    if (OB_FAIL(alloc_data_ptrs()) || OB_ISNULL(data_ptrs_)) {
      ret = OB_INIT_FAIL;
      LOG_WARN("Init ObPUInputStore failed.", K(ret), K(alloc), K(expr->arg_cnt_));
    } else {
      inited_ = true;
      saved_size_ = 0;
    }
  }
  return ret;
}

int ObPUInputStore::alloc_data_ptrs()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(inited_) ) {
    ret = OB_INIT_TWICE;
    LOG_WARN("Repeat allocation.", K(ret));
  } else if (OB_ISNULL(alloc_) || OB_ISNULL(expr_) || expr_->arg_cnt_ <= 0 || length_ <= 0) {
    ret = OB_NOT_INIT;
    LOG_WARN("Uninit allocator and expression.", K(ret));
  } else {
    data_ptrs_ = static_cast<char **>(alloc_->alloc(expr_->arg_cnt_ * sizeof(char *))); // data lists
    if (OB_ISNULL(data_ptrs_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("Allocate data_ptr_ memory failed", K(ret), K(alloc_), K(expr_->arg_cnt_));
    }
    for (int i = 0; i < expr_->arg_cnt_; ++i) {
      ObExpr *e = expr_->args_[i];
      /* allocate by datum type */
      switch(e->datum_meta_.type_) {
        case ObCharType:
        case ObVarcharType:
        case ObTinyTextType:
        case ObTextType:
        case ObMediumTextType:
        case ObLongTextType: {
          data_ptrs_[i] = reinterpret_cast<char *>(alloc_->alloc(length_ * sizeof(char *))); // string lists
          break;
        }
        case ObTinyIntType:
        case ObSmallIntType:
        case ObMediumIntType:
        case ObInt32Type:
        case ObIntType: {
          data_ptrs_[i] = reinterpret_cast<char *>(alloc_->alloc(length_ * sizeof(int))); // int lists
          break;
        }
        case ObDoubleType: {
          data_ptrs_[i] = reinterpret_cast<char *>(alloc_->alloc(length_ * sizeof(double))); // double lists
          break;
        }
        default: {
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("Unsupported input arg type, alloc_data_ptrs failed.", K(ret));
        }
      }
      if (OB_ISNULL(data_ptrs_[i])) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("Allocate data_ptr_[i] memory failed.", K(ret), K(alloc_), K(length_), K(i), K(e->datum_meta_.type_));
      }
    }
  }
  return ret;
}

int ObPUInputStore::reuse()
{
  int ret = OB_SUCCESS;
  saved_size_ = 0;
  return ret;
}

int ObPUInputStore::reset(int64_t length /* default = 0 */)
{
  int ret = OB_SUCCESS;
  if(length <= length_) {
    ret = reuse();
  } else if (OB_FAIL(free())) {
    //
  } else if (OB_FALSE_IT(length_ = length)) {
  } else if (OB_FAIL(alloc_data_ptrs())) {
  } else {
    saved_size_ = 0;
    inited_ = true;
  }
  return ret;
}

int ObPUInputStore::free() {
  int ret = OB_SUCCESS;
  // if expr_ = null, do not check
  for (int i = 0; OB_NOT_NULL(expr_) && i < expr_->arg_cnt_; ++i) {
    ObExpr *e = expr_->args_[i];
    /* allocate by datum type */
    switch(e->datum_meta_.type_) {
      case ObCharType:
      case ObVarcharType:
      case ObTinyTextType:
      case ObTextType:
      case ObMediumTextType:
      case ObLongTextType: {
        alloc_->free(reinterpret_cast<char **>(data_ptrs_[i]));
        break;
      }
      case ObTinyIntType:
      case ObSmallIntType:
      case ObMediumIntType:
      case ObInt32Type:
      case ObIntType: {
        alloc_->free(reinterpret_cast<int *>(data_ptrs_[i]));
        break;
      }
      case ObDoubleType: {
        alloc_->free(reinterpret_cast<double *>(data_ptrs_[i]));
        break;
      }
      default: {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("Unsupported input arg type, free failed in ObPUDataStore.", K(ret));
      }
    }
  }
  return ret;
}

int ObPUInputStore::save_batch(ObEvalCtx &eval_ctx, ObBatchRows &brs)
{
  int ret = OB_SUCCESS;
  int brs_cnt = brs.size_ - brs.skip_->accumulate_bit_cnt(brs.size_);
  if (OB_ISNULL(expr_) || expr_->arg_cnt_ <= 0) {
    // empty exprs_: do nothing
  } else if (OB_ISNULL(data_ptrs_) || !inited_) {
    ret = OB_NOT_INIT;
  } else if (saved_size_ + brs_cnt <= length_) {
    for (int64_t i = 0; i < expr_->arg_cnt_; i++) {
      ObExpr *e = expr_->args_[i];
      ObDatum *src = e->locate_batch_datums(eval_ctx);
      int j = 0, zero = 0;
      int64_t dst_idx = saved_size_;
      int *src_idx;
      if (!expr_->args_[i]->is_const_expr()) 
        src_idx = &j;
      else 
        src_idx = &zero;
      switch(e->datum_meta_.type_) {
        case ObCharType:
        case ObVarcharType:
        case ObTinyTextType:
        case ObTextType:
        case ObMediumTextType:
        case ObLongTextType: {
          /*char **dst = reinterpret_cast<char **>(data_ptrs_[i]);
          for (j = 0; j < brs.size_; j++) {
            if (!brs.skip_->at(j)) {
              // copy string
              ObDatum &src_datum = src[*index];
              char *buf = static_cast<char *>(alloc_->alloc(src_datum.len_));
              MEMCPY(buf, src_datum.ptr_, src_datum.len_);
              //alloc_.free(dst[k]);
              dst[k++] = buf;
            }
          }*/
          PyObject **dst = reinterpret_cast<PyObject **>(data_ptrs_[i]);
          for (j = 0; j < brs.size_; j++) {
            if (!brs.skip_->at(j)) {
              // copy string into pyobject
              ObDatum &src_datum = src[*src_idx];
              PyObject *buf = PyUnicode_FromStringAndSize(src_datum.ptr_, src_datum.len_);
              //alloc_.free(dst[k]);
              dst[dst_idx++] = buf;
            }
          }
          break;
        }
        case ObTinyIntType:
        case ObSmallIntType:
        case ObMediumIntType:
        case ObInt32Type:
        case ObIntType: {
          int *dst = reinterpret_cast<int *>(data_ptrs_[i]);
          for (j = 0; j < brs.size_; j++) {
            ObDatum &src_datum = src[*src_idx];
            if (!brs.skip_->at(j)) {
              // copy int
              dst[dst_idx++] = src_datum.get_int();
            }
          }
          break;
        }
        case ObDoubleType: {
          double *dst = reinterpret_cast<double *>(data_ptrs_[i]);
          for (j = 0; j < brs.size_; j++) {
            ObDatum &src_datum = src[*src_idx];
            if (!brs.skip_->at(j)) {
              // copy double
              dst[dst_idx++] = src_datum.get_double();
            }
          }
          break;
        }
        default: {
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("Unsupported input arg type, save data failed in ObPUDataStore.", K(ret));
        }
      }
      if (OB_ISNULL(data_ptrs_[i])) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("Allocate data_ptr_[i] memory failed.", K(ret), K(alloc_), K(length_), K(i), K(e->datum_meta_.type_));
      }
    }
    saved_size_ += brs_cnt;
  } else {
    // op逻辑控制其不能超过阈值
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ObPUDataStore Stack Overflow.", K(ret));
  }
  return ret;
}

/* ----------------------------------- ObPythonUDFCell --------------------------------- */
int ObPythonUDFCell::init(common::ObIAllocator *alloc, ObExpr *expr, int64_t batch_size, int64_t length)
{
  int ret = OB_SUCCESS;
  // init python udf expr according to its metadata
  if (OB_FAIL(input_store_.init(alloc, expr, length))) {
    ret = OB_NOT_INIT;
    LOG_WARN("Init input store failed.", K(ret));
  } else {
    alloc_ = alloc;
    expr_ = expr;
    //result_store_ = NULL;
    result_size_ = 0;
    batch_size_ = batch_size;
  }
  return ret;
}

int ObPythonUDFCell::free()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(input_store_.free())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Free Python UDF cell failed.", K(ret));
  }
  return ret;
}

int ObPythonUDFCell::do_store(ObEvalCtx &eval_ctx, ObBatchRows &brs)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("Expr in input store is NULL.", K(ret));
  } else {
    // decide save function according to data transfer type
    ObBitVector &my_skip = expr_->get_pvt_skip(eval_ctx);
    for (int64_t i = 0; i < expr_->arg_cnt_ && OB_SUCC(ret); i++) {
      ObExpr *e = expr_->args_[i];
      //ObBitVector &eval_flags = expr.get_evaluated_flags(eval_ctx);
      if (OB_FAIL(e->eval_batch(eval_ctx, my_skip, batch_size_))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Eval Python UDF args' batch result failed.", K(ret));
      }
    }
    // spec_.use_rich_format_ ? input_store_.save_vector(eval_ctx, brs) 
    //                        : input_store_.save_batch(eval_ctx, brs)
    if (OB_FAIL(ret) || OB_FAIL(input_store_.save_batch(eval_ctx, brs))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("Save Python UDF input batch failed.", K(ret), K(expr_), K(batch_size_));
    }
  }
  return ret;
}

int ObPythonUDFCell::do_process()
{
  int ret = OB_SUCCESS;
  // real process
  
  //Ensure GIL
  bool nStatus = PyGILState_Check();
  PyGILState_STATE gstate;
  if(!nStatus) {
    gstate = PyGILState_Ensure();
    nStatus = true;
  }
  //load numpy api
  _import_array();

  PyObject *pArgs = NULL;
  if (OB_FAIL(wrap_input_numpy(pArgs)) || OB_ISNULL(pArgs)) { // wrap the input
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Wrap Cell Input Store as Python UDF input args failed.", K(ret));
  } else if (OB_FAIL(eval(pArgs)) || OB_ISNULL(result_store_)) { // evaluation and keep the result
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Eval Python UDF failed.", K(ret));
  } else {
    input_store_.reuse();
  }
  
  // Release GIL
  if(nStatus)
    PyGILState_Release(gstate);

  return ret;
}

int ObPythonUDFCell::do_restore(ObEvalCtx &eval_ctx, ObBatchRows &brs, int64_t output_idx, int64_t output_size)
{
  int ret = OB_SUCCESS;
  if (output_idx + output_size > result_size_) { //确保访问结果数组时不会越界
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Result Store Overflow.", K(ret));
  } else if (OB_ISNULL(expr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Unexpected NULL expr_.", K(ret));
  } else {
  // } else if (!spec_.use_rich_format_) {
    ObDatum *result_datums = expr_->locate_batch_datums(eval_ctx);
    PyArrayObject *result_store = reinterpret_cast<PyArrayObject *>(result_store_);
    switch(expr_->datum_meta_.type_) {
      case ObCharType:
      case ObVarcharType:
      case ObTinyTextType:
      case ObTextType:
      case ObMediumTextType:
      case ObLongTextType: {
        for (int i = 0; i < output_size; ++i) {
          expr_->reset_ptr_in_datum(eval_ctx, i);
          result_datums[i].set_string(common::ObString(PyUnicode_AsUTF8(
            PyArray_GETITEM(result_store, (char *)PyArray_GETPTR1(result_store, output_idx + i)))));
        }
        break;
      }
      case ObTinyIntType:
      case ObSmallIntType:
      case ObMediumIntType:
      case ObInt32Type:
      case ObIntType: {
        for (int i = 0; i < output_size; ++i) {
          expr_->reset_ptr_in_datum(eval_ctx, i);
          result_datums[i].set_int(PyLong_AsLong(
            PyArray_GETITEM(result_store, (char *)PyArray_GETPTR1(result_store, output_idx + i))));
        }
        break;
      }
      case ObDoubleType: {
        for (int i = 0; i < output_size; ++i) {
          expr_->reset_ptr_in_datum(eval_ctx, i);
          result_datums[i].set_double(PyLong_AsLong(
            PyArray_GETITEM(result_store, (char *)PyArray_GETPTR1(result_store, output_idx + i))));
        }
        break;
      }
      default: {
        //error
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("Unsupported result type.", K(ret));
      }
    }
    // set flags
    //expr_->set_evaluated_flag(eval_ctx);
    expr_->set_evaluated_projected(eval_ctx);
  }
  /* else {
    // construct vector and insert back into the reserved_buf
    
  }*/
  return ret;
}

int ObPythonUDFCell::wrap_input_numpy(PyObject *&pArgs)
{
  int ret = OB_SUCCESS;

  pArgs = PyTuple_New(expr_->arg_cnt_);
  int64_t length = input_store_.get_saved_size();
  npy_intp elements[1] = {length};

  if (OB_ISNULL(expr_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("Expr in input store is NULL.", K(ret));
  } else {
    for (int i = 0; i < expr_->arg_cnt_; ++i) {
      PyObject *numpyarray = NULL;
      switch (expr_->args_[i]->datum_meta_.type_) {
        case ObCharType:
        case ObVarcharType:
        case ObTinyTextType:
        case ObTextType:
        case ObMediumTextType:
        case ObLongTextType: {
          /*numpyarray = PyArray_New(&PyArray_Type, 
                                  1, 
                                  elements, 
                                  NPY_OBJECT, 
                                  NULL, 
                                  reinterpret_cast<PyObject **>(input_store_.get_data_ptr_at(i)), 
                                  length, 
                                  0, 
                                  NULL);*/
          numpyarray = PyArray_New(&PyArray_Type, 1, elements, NPY_OBJECT, NULL, NULL, 0, 0, NULL);
          PyObject **unicode_strs = reinterpret_cast<PyObject **>(input_store_.get_data_ptr_at(i));
          for (int j = 0; j < length; ++j) {
            //put unicode string pyobject into numpy array
            PyArray_SETITEM((PyArrayObject *)numpyarray, (char *)PyArray_GETPTR1((PyArrayObject *)numpyarray, j), 
              unicode_strs[j]);
          }
          break;
        }
        case ObTinyIntType:
        case ObSmallIntType:
        case ObMediumIntType:
        case ObInt32Type:
        case ObIntType: {
          numpyarray = PyArray_New(&PyArray_Type, 
                                  1, 
                                  elements, 
                                  NPY_INT32, 
                                  NULL, 
                                  reinterpret_cast<int *>(input_store_.get_data_ptr_at(i)),  
                                  length, 
                                  0, 
                                  NULL);
          break;
        }
        case ObDoubleType: {
          numpyarray = PyArray_New(&PyArray_Type, 
                                  1, 
                                  elements, 
                                  NPY_FLOAT64, 
                                  NULL, 
                                  reinterpret_cast<double *>(input_store_.get_data_ptr_at(i)), 
                                  length, 
                                  0, 
                                  NULL);
          break;
        }
        default: {
          //error
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("Unsupported input type.", K(ret));
        }
      }
      if(PyTuple_SetItem(pArgs, i, numpyarray) != 0){
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Set numpy array arg failed.", K(ret));
      }
    }
  }
  return ret;
}

int ObPythonUDFCell::eval(PyObject *&pArgs)
{
  int ret = OB_SUCCESS;
  // evalatuion in python interpreter

  // extract pyfun handler
  ObPythonUdfInfo *info = static_cast<ObPythonUdfInfo *>(expr_->extra_info_);
  std::string name(info->udf_meta_.name_.ptr());
  name = name.substr(0, info->udf_meta_.name_.length());
  std::string pyfun_handler = name.append("_pyfun");

  PyObject *pModule = NULL;
  PyObject *pFunc = NULL;
  PyObject *pResult = NULL;

  if (OB_ISNULL(pModule = PyImport_AddModule("__main__"))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Import main module failed.", K(ret));
  } else if (OB_ISNULL(pFunc = PyObject_GetAttrString(pModule, pyfun_handler.c_str())) 
      || !PyCallable_Check(pFunc)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Get function handler failed.", K(ret));
  } else if (OB_ISNULL(pResult = PyObject_CallObject(pFunc, pArgs))) {
    ObExprPythonUdf::process_python_exception();
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Execute Python UDF error.", K(ret));
  } else {
    result_size_ = input_store_.get_saved_size();
    result_store_ = reinterpret_cast<void *>(pResult);
  }

  return ret;
}

/* ----------------------------------- ObPUStoreController --------------------------------- */
int ObPUStoreController::init(int64_t batch_size,
                              const common::ObIArray<ObExpr *> &udf_exprs, 
                              const common::ObIArray<ObExpr *> &input_exprs)
{
  int ret = OB_SUCCESS;
  // init python udf cells
  for (int i = 0; i < udf_exprs.count() && OB_SUCC(ret); ++i) {
    ObExpr *udf_expr = udf_exprs.at(i);
    void *cell_ptr = static_cast<ObPythonUDFCell *>(alloc_.alloc(sizeof(ObPythonUDFCell)));
    ObPythonUDFCell *cell = new (cell_ptr) ObPythonUDFCell();
    if (OB_ISNULL(cell) || OB_FAIL(cell->init(&alloc_, udf_expr, batch_size, DEFAULT_PU_LENGTH)) ) {
      ret = OB_INIT_FAIL;
      LOG_WARN("Init Python UDF Cell failed.", K(ret));
    } else {
      cells_list_.add_last(cell);
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(other_store_.init(input_exprs, batch_size, DEFAULT_PU_LENGTH))) {
      ret = OB_INIT_FAIL;
      LOG_WARN("Init other input store failed.", K(ret));
    } else {
      stored_input_cnt_ = 0;
      stored_output_cnt_ = 0;
      output_idx_ = 0;
      batch_size_ = batch_size;
    }
  }
  return ret;
}

int ObPUStoreController::free()
{
  int ret = OB_SUCCESS;
  ObPythonUDFCell* header = cells_list_.get_header();
  for (ObPythonUDFCell* cell = cells_list_.get_first(); 
       cell != header && OB_SUCC(ret); 
       cell = cell->get_next()) {
    if (OB_FAIL(cell->free())) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("Free Python UDF Cell failed.", K(ret));
      break;
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(other_store_.free())) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("Free other input store failed.", K(ret));
    } else {
      // reset cnt & idx
      stored_input_cnt_ = 0;
      stored_output_cnt_ = 0;
      output_idx_ = 0;
    }
  }
  return ret;
}

int ObPUStoreController::store(ObEvalCtx &eval_ctx, ObBatchRows &brs)
{
  int ret = OB_SUCCESS;
  // do cells input store
  int64_t cnt = brs.size_ - brs.skip_->accumulate_bit_cnt(brs.size_);
  ObPythonUDFCell* header = cells_list_.get_header();
  for (ObPythonUDFCell* cell = cells_list_.get_first(); 
       cell != header && OB_SUCC(ret); 
       cell = cell->get_next()) {
    if (OB_FAIL(cell->do_store(eval_ctx, brs))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("Do Python UDF Cell Store failed.", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    // do other inputs store
    // MY_SPEC.use_rich_format_ ? other_store_->save_vector(eval_ctx, brs)
    //                          : other_store_->save_batch(eval_ctx, brs)
    if (OB_FAIL(other_store_.save_batch(eval_ctx, brs))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("Save other input cols failed.", K(ret));
    } else {
      stored_input_cnt_ += cnt;
    }
  }
  return ret;
}

int ObPUStoreController::process()
{
  int ret = OB_SUCCESS;
  if (is_empty()) {
    /* do nothing */
  } else {
    ObPythonUDFCell* header = cells_list_.get_header();
    for (ObPythonUDFCell* cell = cells_list_.get_first(); 
        cell != header && OB_SUCC(ret); 
        cell = cell->get_next()) {
      if (OB_FAIL(cell->do_process())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Do Python UDF Cell process udf failed.", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      stored_output_cnt_ = stored_input_cnt_;
      stored_input_cnt_ = 0;
      output_idx_ = 0;
    }
  }
  return ret;
}

int ObPUStoreController::restore(ObEvalCtx &eval_ctx, ObBatchRows &brs, int64_t output_size)
{
  int ret = OB_SUCCESS;
  if (stored_output_cnt_ <= 0 || !is_output()) { // Not enough output data
    /* do nothing */
  } else {
    int64_t output_size = batch_size_ < (stored_output_cnt_ - output_idx_) 
                        ? batch_size_ 
                        : (stored_output_cnt_ - output_idx_);
    ObPythonUDFCell* header = cells_list_.get_header();
    for (ObPythonUDFCell* cell = cells_list_.get_first(); 
        cell != header && OB_SUCC(ret); 
        cell = cell->get_next()) {
      if (OB_FAIL(cell->do_restore(eval_ctx, brs, output_idx_, output_size))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Do Python UDF Cell restore failed.", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      // MY_SPEC.use_rich_format_ ? other_store_->load_vector(eval_ctx, brs)
      //                          : other_store_->load_batch(eval_ctx, brs)
      if (OB_FAIL(other_store_.load_batch(eval_ctx, output_size))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Load other input cols failed.", K(ret));
      } else {
        output_idx_ += output_size;
        // reset batchrows
        brs.size_ = output_size;
        brs.reset_skip(output_size);
        brs.end_ = false;
      }
    }
    if (OB_SUCC(ret) && end_output()) {
      other_store_.reuse();
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase