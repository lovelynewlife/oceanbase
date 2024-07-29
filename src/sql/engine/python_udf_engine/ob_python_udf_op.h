#ifndef OCEANBASE_SUBQUERY_OB_PYTHON_UDF_OP_H_
#define OCEANBASE_SUBQUERY_OB_PYTHON_UDF_OP_H_

#include "sql/engine/basic/ob_ra_datum_store.h"
#include "sql/engine/ob_operator.h"
#include "sql/engine/subquery/ob_subplan_scan_op.h"
#include "sql/engine/expr/ob_expr_python_udf.h"


#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <Python.h>
#include <numpy/arrayobject.h>
#include <numpy/npy_common.h>
//#include <Python.h>

namespace oceanbase
{
namespace sql
{

const int DEFAULT_PU_LENGTH = 4096;

// buffer for python_udf, based on VectorStore and BatchResultHolder
struct ObVectorBuffer
{
public:
  ObVectorBuffer() 
  : alloc_(NULL), exprs_(NULL), exec_ctx_(NULL), datums_(NULL),
    max_size_(0), saved_size_(0), inited_(false)
  {}
  int init(common::ObIAllocator *alloc, const common::ObIArray<ObExpr *> &exprs, ObExecContext &exec_ctx, int64_t max_buffer_size);
  int save(ObEvalCtx &eval_ctx, ObBatchRows &brs_);
  bool is_saved() const { return saved_size_ > 0; }
  int get_size() const { return saved_size_; }
  int get_max_size() const { return max_size_; };
  int load(ObEvalCtx &eval_ctx, ObBatchRows &brs_, int64_t &brs_skip_size_, int64_t batch_size);
  int resize(int64_t size);
  int move(int64_t size); // move datums and cut max_size_
private:
  common::ObIAllocator *alloc_;
  const common::ObIArray<ObExpr *> *exprs_;
  ObExecContext *exec_ctx_;
  ObDatum *datums_;
  int64_t max_size_;
  int64_t saved_size_;
  bool inited_;
};

struct ObColInputStore
{
public:
  ObColInputStore(common::ObIAllocator &alloc) 
  : alloc_(alloc), exprs_(alloc), datums_copy_(alloc), length_(0),
    batch_size_(0), saved_size_(0), output_idx_(0), inited_(false)
  {}
  ~ObColInputStore() {}
  int init(const common::ObIArray<ObExpr *> &exprs,
           int64_t batch_size,
           int64_t length);
  int free();
  int reuse();
  int reset(int64_t length = 0);
  int save_batch(ObEvalCtx &eval_ctx, ObBatchRows &brs);
  int save_vector(ObEvalCtx &eval_ctx, ObBatchRows &brs);
  int load_batch(ObEvalCtx &eval_ctx, int64_t load_size);
  int load_vector(ObEvalCtx &eval_ctx, int64_t load_size);

private:
  common::ObIAllocator &alloc_;
  common::ObFixedArray<ObExpr *, common::ObIAllocator> exprs_;
  common::ObFixedArray<ObDatum *, common::ObIAllocator> datums_copy_;
  int64_t length_;
  int64_t batch_size_;
  int64_t saved_size_;
  int64_t output_idx_;
  bool inited_;
};

// store python udf expr input, data is directly sent to python interpreter
struct ObPUInputStore
{
public:
  ObPUInputStore() : alloc_(NULL), expr_(NULL), length_(0), data_ptrs_(NULL), saved_size_(0), inited_(false) {}
  int init(common::ObIAllocator *alloc, ObExpr *expr, int64_t length);
  int alloc_data_ptrs();
  int reuse();
  int reset(int64_t length = 0);
  int free();
  int save_row(ObEvalCtx &eval_ctx, int64_t row_idx); // save row
  int save_batch(ObEvalCtx &eval_ctx, ObBatchRows &brs); // save batch
  int save_vector(ObEvalCtx &eval_ctx, ObBatchRows &brs); // save vector
  //int load(); 
  //int load(int64_t row_idx);
  char** get_data_ptrs() { return data_ptrs_; }
  void* get_data_ptr_at(int64_t i) { // use with expr datum type
    if (i >= 0 && i < expr_->arg_cnt_)
      return reinterpret_cast<void *>(data_ptrs_[i]);
    return nullptr;
  }
  int64_t get_saved_size() {return saved_size_;}

private:
  common::ObIAllocator *alloc_;
  ObExpr *expr_; // Python UDF expr
  int64_t length_;
  char **data_ptrs_;
  int64_t saved_size_;
  bool inited_;
};

class ObPythonUDFCell : public common::ObDLinkBase<ObPythonUDFCell> {
public:
  ObPythonUDFCell() {}
  ~ObPythonUDFCell() {}
  int init(common::ObIAllocator *alloc, ObExpr *expr, int64_t batch_size, int64_t length);
  int free();
  int do_store(ObEvalCtx &eval_ctx, ObBatchRows &brs); // do real storing
  int do_process(); // do real processing
  int do_restore(ObEvalCtx &eval_ctx, ObBatchRows &brs, int64_t output_idx, int64_t output_size);
  int do_restore_vector(ObEvalCtx &eval_ctx, ObBatchRows &brs, int64_t output_idx, int64_t output_size);
  int wrap_input_numpy(PyObject *&pArgs);
  int eval(PyObject *&pArgs); // do python udf evaluation

private:
  common::ObIAllocator *alloc_;
  ObExpr *expr_; // python_udf_expr
  ObPUInputStore input_store_; // 不同UDF间使用同一列存在冗余缓存, 公共表达式部份本来也存在冗余
  int64_t batch_size_; // 系统参数，关系到存取时最大空间
  // 运行时参数...

  // 运算结果暂存
  void *result_store_;
  int result_size_;
};
typedef common::ObDList<ObPythonUDFCell> PythonUDFCellList;


class ObPUStoreController
{
public:
  ObPUStoreController(common::ObIAllocator &alloc) : 
    alloc_(alloc),
    cells_list_(),
    other_store_(alloc),
    ideal_size_(2048),
    stored_input_cnt_(0),
    stored_output_cnt_(0),
    output_idx_(0),
    batch_size_(256) {}
  ~ObPUStoreController() {}
  int init(int64_t batch_size,
           const common::ObIArray<ObExpr *> &udf_exprs, 
           const common::ObIArray<ObExpr *> &input_exprs);
  int free();
  int store(ObEvalCtx &eval_ctx, ObBatchRows &brs);
  int process();
  int restore(ObEvalCtx &eval_ctx, ObBatchRows &brs, int64_t output_size);
  int resize(int64_t size); //扩容
  bool is_full() { return stored_input_cnt_ > ideal_size_; }
  bool is_empty() { return stored_input_cnt_ == 0; }
  bool is_output() { return output_idx_ < stored_output_cnt_; }
  bool end_output() { return output_idx_ == stored_output_cnt_; }

private:
  common::ObIAllocator &alloc_;
  PythonUDFCellList cells_list_;
  ObColInputStore other_store_;

  // 运算过程控制参数
  int64_t ideal_size_;
  int64_t stored_input_cnt_;
  int64_t stored_output_cnt_;
  int64_t output_idx_;

  int64_t batch_size_; // 系统参数，关系到存取时最大空间
};

class ObPythonUDFSpec : public ObOpSpec
{
public:
  OB_UNIS_VERSION_V(1);
public:
  ObPythonUDFSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type);

  ~ObPythonUDFSpec();
  
  //void* _save; //for Python Interpreter Thread State
  ExprFixedArray udf_exprs_; // all python udf rt exprs
  ExprFixedArray input_exprs_; // other input rt exprs(not calculated by python udfs)
};

class ObPythonUDFOp : public ObOperator
{
public:
  ObPythonUDFOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input);

  ~ObPythonUDFOp();

  static int find_predict_size(ObExpr *expr, int32_t &predict_size);

  //virtual int get_next_batch(const int64_t max_row_cnt, const ObBatchRows *&batch_rows) override;

  virtual int inner_open() override;
  virtual int inner_close() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_get_next_batch(const int64_t max_row_cnt) override;
  virtual void destroy() override;

private:
  //int alloc_predict_buffer(ObIAllocator &alloc, ObExpr &expr, ObDatum *&buf_result, int buffer_size);
  //int find_predict_size(ObExpr *expr, int32_t &predict_size);
  int clear_calc_exprs_evaluated_flags();

private:
  /*ExprFixedArray buf_exprs_; //all exprs with fake frames
  nt64_t result_width_; //要进行拷贝的expr数
  int64_t brs_skip_size_;
  ObVectorBuffer input_buffer_;
  ObVectorBuffer output_buffer_;
  ObDatum **buf_results_; // for fake frame
  int predict_size_; //每次python udf计算的元组数
  bool use_input_buf_;
  bool use_output_buf_;
  bool use_fake_frame_;
  bool test_controller_;*/

  common::ObArenaAllocator local_allocator_;
  ObPUStoreController controller_;
};

} // end namespace sql
} // end namespace oceanbase

#endif // OCEANBASE_SUBQUERY_OB_PYTHON_UDF_OP_H_