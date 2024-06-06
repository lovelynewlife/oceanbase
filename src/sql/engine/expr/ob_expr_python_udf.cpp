#define USING_LOG_PREFIX SQL_ENG

#define PY_SSIZE_T_CLEAN
#include <exception>
#include <string.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <sys/syscall.h>

#include "lib/oblog/ob_log.h"

#include "share/object/ob_obj_cast.h"
#include "share/config/ob_server_config.h"
#include "share/datum/ob_datum_util.h"
#include "objit/common/ob_item_type.h"

#include "sql/engine/expr/ob_expr_util.h"
#include "sql/engine/expr/ob_expr_result_type_util.h"
#include "sql/session/ob_sql_session_info.h"

#include "storage/ob_storage_util.h"

#include "sql/engine/expr/ob_expr_python_udf.h"

namespace oceanbase {
using namespace common;
namespace sql {

ObExprPythonUdf::ObExprPythonUdf(ObIAllocator& alloc) : 
  ObExprOperator(alloc, T_FUN_SYS_PYTHON_UDF, N_PYTHON_UDF, MORE_THAN_ZERO), allocator_(alloc), udf_meta_()
{}

ObExprPythonUdf::~ObExprPythonUdf()
{}

int ObExprPythonUdf::calc_result_typeN(ObExprResType &type,
                                       ObExprResType *types_array,
                                       int64_t param_num,
                                       common::ObExprTypeCtx &type_ctx) const 
{
  int ret = OB_SUCCESS;
  UNUSED(param_num);
  UNUSED(types_array);
  switch(udf_meta_.ret_) {
  case share::schema::ObPythonUDF::PyUdfRetType::STRING : 
    type.set_varchar();
    type.set_collation_level(CS_LEVEL_SYSCONST);
    type.set_default_collation_type();
    break;
  case share::schema::ObPythonUDF::PyUdfRetType::DECIMAL :
    type.set_number();
    break;
  case share::schema::ObPythonUDF::PyUdfRetType::INTEGER :
    type.set_int();
    break;
  case share::schema::ObPythonUDF::PyUdfRetType::REAL :
    type.set_double();
    break;
  case share::schema::ObPythonUDF::PyUdfRetType::UDF_UNINITIAL :
    type.set_number();
    break;
  default :
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unhandled python udf result type", K(ret));
  }
  if (OB_SUCC(ret)) {
    type_ctx.set_cast_mode(type_ctx.get_cast_mode() | CM_WARN_ON_FAIL);
  }
  return ret;
}

int ObExprPythonUdf::calc_resultN(common::ObObj &result,
                                  const common::ObObj *objs,
                                  int64_t param_num,
                                  common::ObExprCtx &expr_ctx) const
{
  UNUSED(expr_ctx);
  //result.set_varchar(common::ObString(SAY_HELLO));
  //result.set_collation(result_type_);
  return OB_SUCCESS;
}

int ObExprPythonUdf::set_udf_meta(const share::schema::ObPythonUDFMeta &udf)
{
  return deep_copy_udf_meta(udf_meta_, allocator_, udf);
}

int ObExprPythonUdf::deep_copy_udf_meta(share::schema::ObPythonUDFMeta &dst,
                                     common::ObIAllocator &alloc,
                                     const share::schema::ObPythonUDFMeta &src)
{
  int ret = OB_SUCCESS;
  dst.init_ = src.init_;
  dst.ret_ = src.ret_;
  if (OB_FAIL(ob_write_string(alloc, src.name_, dst.name_))) {
    LOG_WARN("fail to write name", K(src.name_), K(ret));
  } else if (OB_FAIL(ob_write_string(alloc, src.pycall_, dst.pycall_))) {
    LOG_WARN("fail to write pycall", K(src.pycall_), K(ret));
  } else { 
    for (int64_t i = 0; i < src.udf_attributes_types_.count(); i++) {
      dst.udf_attributes_types_.push_back(src.udf_attributes_types_.at(i));
    }
  }
  LOG_DEBUG("set udf meta", K(src), K(dst));
  return ret;
}

int ObExprPythonUdf::init_udf(const common::ObIArray<ObRawExpr*> &param_exprs)
{
  int ret = OB_SUCCESS;
  if (udf_meta_.init_) {
    LOG_DEBUG("udf meta has already inited", K(ret));
    return ret;
  } else if (udf_meta_.ret_ == ObPythonUDF::PyUdfRetType::UDF_UNINITIAL) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("udf meta ret type is null", K(ret));
  } else if (OB_ISNULL(udf_meta_.pycall_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("udf meta pycall is null", K(ret));
  } else {
    // check udf param types
    ARRAY_FOREACH_X(param_exprs, idx, cnt, OB_SUCC(ret)) {
      ObRawExpr *expr = param_exprs.at(idx);
      if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the expr is null", K(ret));
      } else {
        switch(expr->get_result_type().get_type()) {
        case ObCharType :
        case ObVarcharType :
        case ObTinyTextType :
        case ObTextType :
        case ObMediumTextType :
        case ObLongTextType : 
          if(udf_meta_.udf_attributes_types_.at(idx) != ObPythonUDF::STRING) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("the type of param is incorrect", K(ret), K(idx));
          }
          break;
        case ObTinyIntType :
        case ObSmallIntType :
        case ObMediumIntType :
        case ObInt32Type :
        case ObIntType : 
          if(udf_meta_.udf_attributes_types_.at(idx) != ObPythonUDF::INTEGER) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("the type of param is incorrect", K(ret), K(idx));
          }
          break;
        case ObDoubleType : 
          if(udf_meta_.udf_attributes_types_.at(idx) != ObPythonUDF::REAL) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("the type of param is incorrect", K(ret), K(idx));
          }
          break;
        case ObNumberType : 
            //decimal
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("not support decimal", K(ret), K(idx));
            break;
        default : 
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("not support param type", K(ret));
        }
      }
    }
  }
  // check python code
  if (OB_FAIL(ret)) {
    LOG_WARN("Fail to check udf meta", K(ret));
  } else if (import_udf(udf_meta_)) {
    LOG_WARN("Fail to import udf", K(ret));
  } else {
    udf_meta_.init_ = true;
  }
  return ret;
}

int ObExprPythonUdf::import_udf(const share::schema::ObPythonUDFMeta &udf_meta)
{
  int ret = OB_SUCCESS;

  //runtime variables
  PyObject *pModule = NULL;
  PyObject *dic = NULL;
  PyObject *v = NULL;
  PyObject *pInitial = NULL;

  //name
  std::string name(udf_meta.name_.ptr());
  name = name.substr(0, udf_meta.name_.length());
  std::string pyinitial_handler = name + "_pyinitial";
  std::string pyfun_handler = name + "_pyfun";
  //pycall
  std::string pycall(udf_meta.pycall_.ptr());
  pycall = pycall.substr(0, udf_meta.pycall_.length());
  pycall.replace(pycall.find("pyinitial"), 9, pyinitial_handler);
  pycall.replace(pycall.find("pyfun"), 5, pyfun_handler);
  const char* pycall_c = pycall.c_str();
  
  //Acquire GIL
  bool nStatus = PyGILState_Check();
  PyGILState_STATE gstate;
  if(!nStatus) {
    gstate = PyGILState_Ensure();
    nStatus = true;
  }

  // prepare and import python code
  pModule = PyImport_AddModule("__main__"); // load main module
  if(OB_ISNULL(pModule)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to import main module", K(ret));
    goto destruction;
  }
  dic = PyModule_GetDict(pModule); // get main module dic
  if(OB_ISNULL(dic)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get main module dic", K(ret));
    goto destruction;
  } 
  v = PyRun_StringFlags(pycall_c, Py_file_input, dic, dic, NULL); // test pycall
  if(OB_ISNULL(v)) {
    process_python_exception();
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to write pycall into module", K(ret));
    goto destruction;
  }
  pInitial = PyObject_GetAttrString(pModule, pyinitial_handler.c_str()); // get pyInitial()
  if(OB_ISNULL(pInitial) || !PyCallable_Check(pInitial)) {
    process_python_exception();
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Fail to import pyinitial", K(ret));
    goto destruction;
  } else if (OB_ISNULL(PyObject_CallObject(pInitial, NULL))){
    process_python_exception();
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Fail to run pyinitial", K(ret));
    goto destruction;
  } else {
    LOG_DEBUG("Import python udf handler", K(ret));
  }

  destruction: 
  //release GIL
  if(nStatus)
    PyGILState_Release(gstate);

  return ret;
}

int ObExprPythonUdf::eval_test_udf(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum) {
  int ret = OB_SUCCESS;

  //extract pyfun handler
  const ObPythonUdfInfo *info = static_cast<ObPythonUdfInfo *>(expr.extra_info_);
  std::string name(info->udf_meta_.name_.ptr());
  name = name.substr(0, info->udf_meta_.name_.length());
  std::string pyfun_handler = name.append("_pyfun");

  //Ensure GIL
  bool nStatus = PyGILState_Check();
  PyGILState_STATE gstate;
  if(!nStatus) {
    gstate = PyGILState_Ensure();
    nStatus = true;
  }

  //load numpy api
  _import_array(); 

  //运行时变量
  PyObject *pModule = NULL;
  PyObject *pFunc = NULL;
  PyObject *pArgs = PyTuple_New(expr.arg_cnt_);
  PyObject *pResult = NULL;
  PyObject *numpyarray = NULL;
  ObEvalCtx::TempAllocGuard alloc_guard(ctx);
  ObIAllocator &tmp_alloc = alloc_guard.get_allocator(); 
  PyObject **arrays = (PyObject **)tmp_alloc.alloc(sizeof(PyObject *) * expr.arg_cnt_);
  for(int i = 0; i < expr.arg_cnt_; i++)
    arrays[i] = NULL;
  npy_intp elements[1] = {1};
  ObDatum *argDatum = NULL;

  //获取udf实例并核验
  pModule = PyImport_AddModule("__main__");
  if(OB_ISNULL(pModule)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Fail to import main module", K(ret));
    goto destruction;
  }
  pFunc = PyObject_GetAttrString(pModule, pyfun_handler.c_str());
  if(OB_ISNULL(pFunc) || !PyCallable_Check(pFunc)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Fail to get function handler", K(ret));
    goto destruction;
  }

  //传递udf运行时参数
  for(int i = 0;i < expr.arg_cnt_;i++) {
    //get args from expr
    if(expr.args_[i]->eval(ctx, argDatum) != OB_SUCCESS){
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to obtain arg", K(ret));
      goto destruction;
    }
    //转换得到numpy array --> 单一元素
    switch(expr.args_[i]->datum_meta_.type_) {
      case ObCharType:
      case ObVarcharType:
      case ObTinyTextType:
      case ObTextType:
      case ObMediumTextType:
      case ObLongTextType: {
        //str in OB
        ObString str = argDatum->get_string();
        //put str into numpy array
        numpyarray = PyArray_New(&PyArray_Type, 1, elements, NPY_OBJECT, NULL, NULL, 0, 0, NULL);
        PyArray_SETITEM((PyArrayObject *)numpyarray, (char *)PyArray_GETPTR1((PyArrayObject *)numpyarray, 0), 
          PyUnicode_FromStringAndSize(str.ptr(), str.length()));
        break;
      }
      case ObTinyIntType:
      case ObSmallIntType:
      case ObMediumIntType:
      case ObInt32Type:
      case ObIntType: {
        //put integer into numpy array
        numpyarray = PyArray_EMPTY(1, elements, NPY_INT32, 0);
        PyArray_SETITEM((PyArrayObject *)numpyarray, (char *)PyArray_GETPTR1((PyArrayObject *)numpyarray, 0), PyLong_FromLong(argDatum->get_int()));
        break;
      }
      case ObDoubleType: {
        //put double into numpy array
        numpyarray = PyArray_EMPTY(1, elements, NPY_FLOAT64, 0);
        PyArray_SETITEM((PyArrayObject *)numpyarray, (char *)PyArray_GETPTR1((PyArrayObject *)numpyarray, 0), PyFloat_FromDouble(argDatum->get_double()));
        break;
      }
      case ObNumberType: {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("number type, fail in obdatum2array", K(ret));
        goto destruction;
      }
      default: {
        //error
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unknown arg type, fail in obdatum2array", K(ret));
        goto destruction;
      }
    }
    //插入pArg
    if(PyTuple_SetItem(pArgs, i, numpyarray) != 0){
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to set numpy array arg", K(ret));
      goto destruction;
    }
    arrays[i] = numpyarray;
  }

  //执行Python Code并获取返回值
  pResult = PyObject_CallObject(pFunc, pArgs);
  if(!pResult){
    process_python_exception();
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("execute error", K(ret));
    goto destruction;
  }

  //根据类型从numpy数组中取出返回值并填入返回值
  switch (expr.datum_meta_.type_)
  {
    case ObCharType:
    case ObVarcharType:
    case ObTinyTextType:
    case ObTextType:
    case ObMediumTextType:
    case ObLongTextType: {
      expr_datum.set_string(common::ObString(PyUnicode_AsUTF8(
        PyArray_GETITEM((PyArrayObject *)pResult, (char *)PyArray_GETPTR1((PyArrayObject *)pResult, 0)))));
      break;
    }
    case ObTinyIntType:
    case ObSmallIntType:
    case ObMediumIntType:
    case ObInt32Type:
    case ObIntType: {
      expr_datum.set_int(PyLong_AsLong(
        PyArray_GETITEM((PyArrayObject *)pResult, (char *)PyArray_GETPTR1((PyArrayObject *)pResult, 0))));
      break;
    }
    case ObDoubleType:{
      expr_datum.set_double(PyFloat_AsDouble(
        PyArray_GETITEM((PyArrayObject *)pResult, (char *)PyArray_GETPTR1((PyArrayObject *)pResult, 0))));
      break;
    }
    case ObNumberType: {
      //error
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("not support ObNumberType", K(ret));
      goto destruction;
    }
    default: {
      //error
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unknown result type", K(ret));
      goto destruction;
    }
  }

  //释放资源
  destruction:
  //释放运行时变量
  Py_XDECREF(pArgs);
  //释放函数参数
  for (int i = 0; i < expr.arg_cnt_; i++) {
    if(OB_ISNULL(arrays[i]))
      continue;
    else
      PyArray_XDECREF((PyArrayObject *)arrays[i]);
  }
  //释放计算结果
  if(pResult != NULL) {
    Py_XDECREF(pResult);
  }

  //PyGC_Enable();
  //PyGC_Collect();
  
  //release GIL
  if(nStatus)
    PyGILState_Release(gstate);

  return ret;
}

int ObExprPythonUdf::eval_python_udf_batch_pyobject(const ObExpr &expr, ObEvalCtx &ctx,
                                                    const ObBitVector &skip, const int64_t batch_size) {
  int ret = OB_SUCCESS;
  
  // 开始统计时间
  struct timeval t1, t2, t3, t4, t5, t6;
  double timeuse;
  gettimeofday(&t1, NULL);

  //extract pyfun handler
  ObPythonUdfInfo *info = static_cast<ObPythonUdfInfo *>(expr.extra_info_);
  std::string name(info->udf_meta_.name_.ptr());
  name = name.substr(0, info->udf_meta_.name_.length());
  std::string pyfun_handler = name.append("_pyfun");

  //返回值
  ObDatum *results = expr.locate_batch_datums(ctx);

  //eval and check params
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObBitVector &my_skip = expr.get_pvt_skip(ctx);
  my_skip.deep_copy(skip, batch_size);
  for (int i = 0; i < expr.arg_cnt_; i++) {
    if (expr.args_[i]->is_const_expr())
      continue;
    //do eval
    if (OB_FAIL(expr.args_[i]->eval_batch(ctx, my_skip, batch_size))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to eval batch result args", K(ret));
      return ret;
    }
    //do check
    ObDatum *datum_array = expr.args_[i]->locate_batch_datums(ctx);
    for (int j = 0; j < batch_size; j++) {
      if (my_skip.at(j) || eval_flags.at(j))
        continue;
      else if (datum_array[j].is_null()) {
        //存在null推理结果即为空
        results[j].set_null();
        my_skip.set(j);
        eval_flags.set(j);
      }
    }
  }
  int64_t real_param = 0;
  for (int i = 0; i < batch_size; i++) {
    if (my_skip.at(i) || eval_flags.at(i))
      continue;
    else
      ++real_param;
  }

  //Ensure GIL
  bool nStatus = PyGILState_Check();
  PyGILState_STATE gstate;
  if(!nStatus) {
    gstate = PyGILState_Ensure();
    nStatus = true;
  }

  PyObject *pModule = NULL;
  PyObject *pFunc = NULL;
  PyObject *pArgs = PyTuple_New(expr.arg_cnt_);
  PyObject *pKwargs = PyDict_New();
  PyObject *pResult = NULL;
  PyObject *argTuple = NULL;
  ObDatum *argDatum = NULL;
  
  /*if(info->udf_meta_.init_) {
  } else if (OB_FAIL(import_udf(info->udf_meta_))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Fail to import udf", K(ret));
    goto destruction;
  } else {
    info->udf_meta_.init_ = true;
  }*/

  //获取udf实例并核验
  pModule = PyImport_AddModule("__main__");
  if (OB_ISNULL(pModule)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Fail to import main module", K(ret));
    goto destruction;
  }
  
  pFunc = PyObject_GetAttrString(pModule, pyfun_handler.c_str());
  if (OB_ISNULL(pFunc) || !PyCallable_Check(pFunc)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Fail to get function handler", K(ret));
    goto destruction;
  }

  gettimeofday(&t2, NULL);

  int k, ret_size;
  //传递udf运行时参数
  for (int i = 0;i < expr.arg_cnt_;i++) {
    k = 0;
    argDatum = expr.args_[i]->locate_batch_datums(ctx);
    argTuple = PyTuple_New(real_param); // row size
    int j = 0, zero = 0;
    int *index;
    if (!expr.args_[i]->is_const_expr()) 
      index = &j;
    else 
      index = &zero;
    switch (expr.args_[i]->datum_meta_.type_) {
      case ObCharType:
      case ObVarcharType:
      case ObTinyTextType:
      case ObTextType:
      case ObMediumTextType:
      case ObLongTextType: {
        for (j = 0; j < batch_size; j++) {
          if (my_skip.at(j) || eval_flags.at(j))
            continue;
          else {
            //str in OB
            ObString str = argDatum[*index].get_string();
            //put str into pyobject array
            PyTuple_SetItem(argTuple, k++, PyUnicode_FromStringAndSize(str.ptr(), str.length()));
          }
        }
        break;
      }
      case ObTinyIntType:
      case ObSmallIntType:
      case ObMediumIntType:
      case ObInt32Type:
      case ObIntType: {
        for (j = 0; j < batch_size; j++) {
          if (my_skip.at(j) || eval_flags.at(j))
            continue;
          else
            //put integer into pyobject array
            PyTuple_SetItem(argTuple, k++, PyLong_FromLong(argDatum[*index].get_int()));
        }
        break;
      }
      case ObDoubleType: {
        for (j = 0; j < batch_size; j++) {
          if (my_skip.at(j) || eval_flags.at(j))
            continue;
          else
            //put double into pyobject array
            PyTuple_SetItem(argTuple, k++, PyLong_FromLong(argDatum[*index].get_double()));
        }
        break;
      }
      case ObNumberType: {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("number type, fail in obdatum2array", K(ret));
        goto destruction;
      }
      default: {
        //error
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unknown arg type, fail in obdatum2array", K(ret));
        goto destruction;
      }
    }
    //插入pArg
    if(PyTuple_SetItem(pArgs, i, argTuple) != 0){
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to set pyobject array arg", K(ret));
      goto destruction;
    }
  }

  gettimeofday(&t3, NULL);

  //执行Python Code并获取返回值
  pResult = PyObject_CallObject(pFunc, pArgs);
  if (!pResult) {
    process_python_exception();
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("execute error", K(ret));
    goto destruction;
  }

  gettimeofday(&t4, NULL);

  //根据类型从numpy数组中取出返回值并填入返回值
  k = 0;
  ret_size = PyTuple_Size(pResult);
  switch (expr.datum_meta_.type_)
  {
    case ObCharType:
    case ObVarcharType:
    case ObTinyTextType:
    case ObTextType:
    case ObMediumTextType:
    case ObLongTextType: {
      for (int j = 0; j < batch_size && k < ret_size; j++) {
        if (my_skip.at(j) || eval_flags.at(j))
          continue;
        results[j].set_string(common::ObString(PyUnicode_AsUTF8(
          PyTuple_GetItem(pResult, k++))));
      }
      break;
    }
    case ObTinyIntType:
    case ObSmallIntType:
    case ObMediumIntType:
    case ObInt32Type:
    case ObIntType: {
      for (int j = 0; j < batch_size && k < ret_size; j++) {
        if (my_skip.at(j) || eval_flags.at(j))
          continue;
        results[j].set_int(PyLong_AsLong(
          PyTuple_GetItem(pResult, k++)));
      }
      break;
    }
    case ObDoubleType:{
      for (int j = 0; j < batch_size && k < ret_size; j++) {
        if (my_skip.at(j) || eval_flags.at(j))
          continue;
        results[j].set_double(PyFloat_AsDouble(
          PyTuple_GetItem(pResult, k++)));
      }
      break;
    }
    case ObNumberType: {
      //error
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("not support ObNumberType", K(ret));
      goto destruction;
    }
    default: {
      //error
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unknown result type", K(ret));
      goto destruction;
    }
  }
  gettimeofday(&t5, NULL);

  //释放资源
  destruction:
  //释放运行时变量
  Py_XDECREF(pKwargs);
  //释放函数参数
  Py_XDECREF(pArgs);
  //释放计算结果
  Py_XDECREF(pResult);

  //PyGC_Enable();
  //PyGC_Collect();

  //release GIL
  if(nStatus)
    PyGILState_Release(gstate);
  
  // 计算运行时间，调整predict size | pjx
  /*gettimeofday(&t6, NULL);
  timeuse = (t6.tv_sec - t1.tv_sec) * 1000000 + (double)(t6.tv_usec - t1.tv_usec); // usec
  double tps = real_param * 1000000 / timeuse; // current tuples per sec
  if (info->tps_s == 0) { // 初始化
    info->tps_s = tps;
    info->predict_size += info->delta; // 尝试调整
  } else if (info->round > info->round_limit || real_param != info->predict_size) { //超过轮次，停止调整batch size
    // do nothing
  } else if (tps > (1 + info->lambda) * info->tps_s) { 
    // 提升阈值λ为10% 且 目前计算数量与给定batch size相符，重置轮次
    info->tps_s = tps;
    info->predict_size += info->delta;
    info->round = 0;
  } else if (tps < info->tps_s * (1 - 0.0)) { // 未达到阈值， 且差距较大 ，减小到达阈值的难度，提升轮次
    // 降低阈值σ
    info->tps_s = (1 - info->alpha) * info->tps_s + info->alpha * tps; // 平滑系数α
    info->round++;
  }*/
  
  // 计算运行时间，调整predict size | zcy
  bool start_query = false;
  gettimeofday(&t6, NULL);
  timeuse = (t6.tv_sec - t1.tv_sec) * 1000000 + (double)(t6.tv_usec - t1.tv_usec); // usec
  double tps = real_param * 1000000 / timeuse; // current tuples per sec
  if (info->tps_s == 0) { // 初始化
    info->tps_s = tps;
    info->predict_size += info->delta; // 尝试调整
    start_query = true;
  } else if (info->round > info->round_limit || real_param != info->predict_size) { //超过轮次，停止调整batch size 或 不符合predict size
    // do nothing
  } else if (tps > info->tps_s) { 
    // 提升阈值λ为10% 且 目前计算数量与给定batch size相符，进行调整
    if (tps < (1 + info->lambda) * info->tps_s)
      info->round++;
    info->tps_s = tps;
    info->predict_size += info->delta;
  } else { //tps <= info->tps_s
    // 未达到阈值
    info->tps_s = (1 - info->alpha) * info->tps_s + info->alpha * tps; // 平滑系数α
    if (tps > (1 - info->lambda) * info->tps_s)
      info->round++;
  }
  
  // 插桩 记录运行时间
  /*double inference_time = (t4.tv_sec - t3.tv_sec) * 1000 + (double)(t4.tv_usec - t3.tv_usec) / 1000;
  std::string file_name("/home/test/log/");
  file_name.append(std::string(info->udf_meta_.name_.ptr(), info->udf_meta_.name_.length()));
  file_name.append(".log");
  std::fstream f;
  f.open(file_name, std::ios::out | std::ios::app); // 追加写入
  if (start_query)
    f << "Start a new Query!" << std::endl;
  f << "inference batch size: " << real_param << std::endl;
  f << "execution time: " << timeuse/1000 << " ms" << std::endl;
  f << "pre process time: " << (t2.tv_sec - t1.tv_sec) * 1000 + (double)(t2.tv_usec - t1.tv_usec) / 1000 << " ms" << std::endl;
  f << "ob_py transformation time: " << (t3.tv_sec - t2.tv_sec) * 1000 + (double)(t3.tv_usec - t2.tv_usec) / 1000 << " ms" << std::endl;
  f << "inference time: " << inference_time << " ms" << std::endl;
  f << "py_ob transformation time: " << (t5.tv_sec - t4.tv_sec) * 1000 + (double)(t5.tv_usec - t4.tv_usec) / 1000 << " ms" << std::endl;
  f << "after process time: " << (t6.tv_sec - t5.tv_sec) * 1000 + (double)(t6.tv_usec - t5.tv_usec) / 1000 << " ms" << std::endl;
  f << "tuples per second: " << tps << std::endl;
  f << "tps* : " << info->tps_s << std::endl;
  f << std::endl;
  f.close();*/
  return ret;
}

int ObExprPythonUdf::eval_test_udf_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                         const ObBitVector &skip, const int64_t batch_size) {
  int ret = OB_SUCCESS;
  
  // 开始统计时间
  struct timeval t1, t2, t3, t4, t5, t6;
  double timeuse;
  gettimeofday(&t1, NULL);

  //extract pyfun handler
  ObPythonUdfInfo *info = static_cast<ObPythonUdfInfo *>(expr.extra_info_);
  std::string name(info->udf_meta_.name_.ptr());
  name = name.substr(0, info->udf_meta_.name_.length());
  std::string pyfun_handler = name.append("_pyfun");

  //返回值
  ObDatum *results = expr.locate_batch_datums(ctx);

  //eval and check params
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObBitVector &my_skip = expr.get_pvt_skip(ctx);
  my_skip.deep_copy(skip, batch_size);
  for (int i = 0; i < expr.arg_cnt_; i++) {
    //do eval
    if (OB_FAIL(expr.args_[i]->eval_batch(ctx, my_skip, batch_size))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to eval batch result args", K(ret));
      return ret;
    }
    //do check
    ObDatum *datum_array = expr.args_[i]->locate_batch_datums(ctx);
    for (int j = 0; j < batch_size; j++) {
      if (my_skip.at(j) || eval_flags.at(j))
        continue;
      else if (datum_array[j].is_null()&&!expr.args_[i]->is_const_expr()) {
        //存在null推理结果即为空
        results[j].set_null();
        my_skip.set(j);
        eval_flags.set(j);
      }
    }
  }
  int64_t real_param = 0;
  for (int i = 0; i < batch_size; i++) {
    if (my_skip.at(i) || eval_flags.at(i))
      continue;
    else
      ++real_param;
  }

  //Ensure GIL
  bool nStatus = PyGILState_Check();
  PyGILState_STATE gstate;
  if(!nStatus) {
    gstate = PyGILState_Ensure();
    nStatus = true;
  }

  //load numpy api
  _import_array(); 

  //运行时变量
  ObEvalCtx::TempAllocGuard alloc_guard(ctx);
  ObIAllocator &tmp_alloc = alloc_guard.get_allocator(); 
  //tmp_alloc.reset();
  PyObject **arrays = (PyObject **)tmp_alloc.alloc(sizeof(PyObject *) * expr.arg_cnt_);
  //PyObject **arrays = (PyObject **)ctx.allocator_.alloc(sizeof(PyObject *) * expr.arg_cnt_);
  //PyObject **arrays = new PyObject* [expr.arg_cnt_];
  if (arrays == nullptr) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Fail to allocate numpy arrays", K(ret));
    return ret;
  } else {
    for(int i = 0; i < expr.arg_cnt_; i++)
      arrays[i] = NULL;
  }

  PyObject *pModule = NULL;
  PyObject *pFunc = NULL;
  PyObject *pArgs = PyTuple_New(expr.arg_cnt_);
  PyObject *pKwargs = PyDict_New();
  PyObject *pResult = NULL;
  PyObject *numpyarray = NULL;
  npy_intp elements[1] = {real_param}; // row size
  ObDatum *argDatum = NULL;
  
  //获取udf实例并核验
  pModule = PyImport_AddModule("__main__");
  if (OB_ISNULL(pModule)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Fail to import main module", K(ret));
    goto destruction;
  }
  
  pFunc = PyObject_GetAttrString(pModule, pyfun_handler.c_str());
  if (OB_ISNULL(pFunc) || !PyCallable_Check(pFunc)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Fail to get function handler", K(ret));
    goto destruction;
  }

  gettimeofday(&t2, NULL);

  int k, ret_size;
  //传递udf运行时参数
  for (int i = 0;i < expr.arg_cnt_;i++) {
    k = 0;
    argDatum = expr.args_[i]->locate_batch_datums(ctx);
    //转换得到numpy array --> 单一元素
    int j = 0, zero = 0;
    int *index;
    if (!expr.args_[i]->is_const_expr()) 
      index = &j;
    else 
      index = &zero;
    switch (expr.args_[i]->datum_meta_.type_) {
      case ObCharType:
      case ObVarcharType:
      case ObTinyTextType:
      case ObTextType:
      case ObMediumTextType:
      case ObLongTextType: {
        numpyarray = PyArray_New(&PyArray_Type, 1, elements, NPY_OBJECT, NULL, NULL, 0, 0, NULL);
        for (j = 0; j < batch_size; j++) {
          if (my_skip.at(j) || eval_flags.at(j))
            continue;
          else {
            //str in OB
            ObString str = argDatum[*index].get_string();
            //put str into numpy array
            PyArray_SETITEM((PyArrayObject *)numpyarray, (char *)PyArray_GETPTR1((PyArrayObject *)numpyarray, k++), 
              PyUnicode_FromStringAndSize(str.ptr(), str.length()));
          }
        }
        break;
      }
      case ObTinyIntType:
      case ObSmallIntType:
      case ObMediumIntType:
      case ObInt32Type:
      case ObIntType: {
        numpyarray = PyArray_EMPTY(1, elements, NPY_INT32, 0);
        for (j = 0; j < batch_size; j++) {
          if (my_skip.at(j) || eval_flags.at(j))
            continue;
          //put integer into numpy array
          if(expr.args_[i]->is_const_expr()&&!expr.args_[i]->is_batch_result()){
            int tmp2=argDatum[0].get_int();
            PyArray_SETITEM((PyArrayObject *)numpyarray, (char *)PyArray_GETPTR1((PyArrayObject *)numpyarray, k++), PyLong_FromLong(argDatum[0].get_int()));
          }else{
            int tmp3=argDatum[j].get_int();
            PyArray_SETITEM((PyArrayObject *)numpyarray, (char *)PyArray_GETPTR1((PyArrayObject *)numpyarray, k++), PyLong_FromLong(argDatum[j].get_int()));
          }
        }
        /*int *pint = (int *)tmp_alloc.alloc(sizeof(int32_t) * real_param);
        for (j = 0; j < batch_size; j++) {
          if (my_skip.at(j) || eval_flags.at(j))
            continue;
          else
            //put integer into numpy array
            //PyArray_SETITEM((PyArrayObject *)numpyarray, (char *)PyArray_GETPTR1((PyArrayObject *)numpyarray, k++), PyLong_FromLong(argDatum[*index].get_int()));
            pint[k++] = argDatum[*index].get_int();
        }
        numpyarray = PyArray_New(&PyArray_Type, 1, elements, NPY_INT32, NULL, pint, real_param, 0, NULL);*/
        break;
      }
      case ObDoubleType: {
        //numpyarray = PyArray_EMPTY(1, elements, NPY_FLOAT64, 0);
        double *pdouble = (double *)tmp_alloc.alloc(sizeof(double) * real_param);
        for (j = 0; j < batch_size; j++) {
          if (my_skip.at(j) || eval_flags.at(j))
            continue;
          else
            //put double into numpy array
            //PyArray_SETITEM((PyArrayObject *)numpyarray, (char *)PyArray_GETPTR1((PyArrayObject *)numpyarray, k++), PyFloat_FromDouble(argDatum[*index].get_double()));
            pdouble[k++] = argDatum[*index].get_double();
        }
        numpyarray = PyArray_New(&PyArray_Type, 1, elements, NPY_FLOAT32, NULL, pdouble, real_param, 0, NULL);
        break;
      }
      case ObNumberType: {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("number type, fail in obdatum2array", K(ret));
        goto destruction;
      }
      default: {
        //error
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unknown arg type, fail in obdatum2array", K(ret));
        goto destruction;
      }
    }
    //插入pArg
    if(PyTuple_SetItem(pArgs, i, numpyarray) != 0){
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to set numpy array arg", K(ret));
      goto destruction;
    }
    arrays[i] = numpyarray;
  }

  gettimeofday(&t3, NULL);

  //执行Python Code并获取返回值
  pResult = PyObject_CallObject(pFunc, pArgs);
  if (!pResult) {
    process_python_exception();
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("execute error", K(ret));
    goto destruction;
  }

  gettimeofday(&t4, NULL);

  //根据类型从numpy数组中取出返回值并填入返回值
  ret_size = PyArray_SIZE((PyArrayObject *)pResult);
  k = 0;
  switch (expr.datum_meta_.type_)
  {
    case ObCharType:
    case ObVarcharType:
    case ObTinyTextType:
    case ObTextType:
    case ObMediumTextType:
    case ObLongTextType: {
      for (int j = 0; j < batch_size && k < ret_size; j++) {
        if (my_skip.at(j) || eval_flags.at(j))
          continue;
        results[j].set_string(common::ObString(PyUnicode_AsUTF8(
          PyArray_GETITEM((PyArrayObject *)pResult, (char *)PyArray_GETPTR1((PyArrayObject *)pResult, k++)))));
      }
      break;
    }
    case ObTinyIntType:
    case ObSmallIntType:
    case ObMediumIntType:
    case ObInt32Type:
    case ObIntType: {
      for (int j = 0; j < batch_size && k < ret_size; j++) {
        if (my_skip.at(j) || eval_flags.at(j))
          continue;
        int tmp=PyLong_AsLong(
          PyArray_GETITEM((PyArrayObject *)pResult, (char *)PyArray_GETPTR1((PyArrayObject *)pResult, k++)));
        results[j].set_int(tmp);
      }
      break;
    }
    case ObDoubleType:{
      for (int j = 0; j < batch_size && k < ret_size; j++) {
        if (my_skip.at(j) || eval_flags.at(j))
          continue;
        results[j].set_double(PyFloat_AsDouble(
          PyArray_GETITEM((PyArrayObject *)pResult, (char *)PyArray_GETPTR1((PyArrayObject *)pResult, k++))));
      }
      break;
    }
    case ObNumberType: {
      //error
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("not support ObNumberType", K(ret));
      goto destruction;
    }
    default: {
      //error
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unknown result type", K(ret));
      goto destruction;
    }
  }
  gettimeofday(&t5, NULL);

  //释放资源
  destruction:
  //释放运行时变量
  Py_XDECREF(pKwargs);
  //释放函数参数
  for (int i = 0; i < expr.arg_cnt_; i++) {
    if(arrays[i] == NULL)
      continue;
    else {
      PyArray_XDECREF((PyArrayObject *)arrays[i]);
      //arrays[i] = NULL;
    }
  }
  //arrays = NULL;
  //delete[] arrays;
  Py_XDECREF(pArgs);
  //释放计算结果
  if(pResult != NULL) {
    PyArray_XDECREF((PyArrayObject *)pResult);
    Py_XDECREF(pResult);
  }

  //PyGC_Enable();
  //PyGC_Collect();

  //release GIL
  if(nStatus)
    PyGILState_Release(gstate);
  
  // 计算运行时间，调整predict size | pjx
  /*gettimeofday(&t6, NULL);
  timeuse = (t6.tv_sec - t1.tv_sec) * 1000000 + (double)(t6.tv_usec - t1.tv_usec); // usec
  double tps = real_param * 1000000 / timeuse; // current tuples per sec
  if (info->tps_s == 0) { // 初始化
    info->tps_s = tps;
    info->predict_size += info->delta; // 尝试调整
  } else if (info->round > info->round_limit || real_param != info->predict_size) { //超过轮次，停止调整batch size
    // do nothing
  } else if (tps > (1 + info->lambda) * info->tps_s) { 
    // 提升阈值λ为10% 且 目前计算数量与给定batch size相符，重置轮次
    info->tps_s = tps;
    info->predict_size += info->delta;
    info->round = 0;
  } else if (tps < info->tps_s * (1 - 0.0)) { // 未达到阈值， 且差距较大 ，减小到达阈值的难度，提升轮次
    // 降低阈值σ
    info->tps_s = (1 - info->alpha) * info->tps_s + info->alpha * tps; // 平滑系数α
    info->round++;
  }*/
  
  // 计算运行时间，调整predict size | zcy
  bool start_query = false;
  gettimeofday(&t6, NULL);
  timeuse = (t6.tv_sec - t1.tv_sec) * 1000000 + (double)(t6.tv_usec - t1.tv_usec); // usec
  double tps = real_param * 1000000 / timeuse; // current tuples per sec
  if (info->tps_s == 0) { // 初始化
    info->tps_s = tps;
    info->predict_size += info->delta; // 尝试调整
    start_query = true;
  } else if (info->round > info->round_limit || real_param != info->predict_size) { //超过轮次，停止调整batch size 或 不符合predict size
    // do nothing
  } else if (tps > info->tps_s) { 
    // 提升阈值λ为10% 且 目前计算数量与给定batch size相符，进行调整
    if (tps < (1 + info->lambda) * info->tps_s)
      info->round++;
    info->tps_s = tps;
    info->predict_size += info->delta;
  } else { //tps <= info->tps_s
    // 未达到阈值
    info->tps_s = (1 - info->alpha) * info->tps_s + info->alpha * tps; // 平滑系数α
    if (tps > (1 - info->lambda) * info->tps_s)
      info->round++;
  }
  
  // 插桩 记录运行时间
  /*double inference_time = (t4.tv_sec - t3.tv_sec) * 1000 + (double)(t4.tv_usec - t3.tv_usec) / 1000;
  std::string file_name("/home/test/log/");
  file_name.append(std::string(info->udf_meta_.name_.ptr(), info->udf_meta_.name_.length()));
  file_name.append(".log");
  std::fstream f;
  f.open(file_name, std::ios::out | std::ios::app); // 追加写入
  if (start_query)
    f << "Start a new Query!" << std::endl;
  f << "inference batch size: " << real_param << std::endl;
  f << "execution time: " << timeuse/1000 << " ms" << std::endl;
  f << "pre process time: " << (t2.tv_sec - t1.tv_sec) * 1000 + (double)(t2.tv_usec - t1.tv_usec) / 1000 << " ms" << std::endl;
  f << "ob_py transformation time: " << (t3.tv_sec - t2.tv_sec) * 1000 + (double)(t3.tv_usec - t2.tv_usec) / 1000 << " ms" << std::endl;
  f << "inference time: " << inference_time << " ms" << std::endl;
  f << "py_ob transformation time: " << (t5.tv_sec - t4.tv_sec) * 1000 + (double)(t5.tv_usec - t4.tv_usec) / 1000 << " ms" << std::endl;
  f << "after process time: " << (t6.tv_sec - t5.tv_sec) * 1000 + (double)(t6.tv_usec - t5.tv_usec) / 1000 << " ms" << std::endl;
  f << "tuples per second: " << tps << std::endl;
  f << "tps* : " << info->tps_s << std::endl;
  f << std::endl;
  f.close();*/
  return ret;
}


int ObExprPythonUdf::cg_expr(ObExprCGCtx& expr_cg_ctx, const ObRawExpr& raw_expr, ObExpr& rt_expr) const
{
  int ret = OB_SUCCESS;
  //Python UDF Extra Info
  ObIAllocator &alloc = *expr_cg_ctx.allocator_;
  const ObPythonUdfRawExpr &fun_sys = static_cast<const ObPythonUdfRawExpr &>(raw_expr);
  ObPythonUdfInfo *info = OB_NEWx(ObPythonUdfInfo, (&alloc),
                                   alloc, T_FUN_SYS_PYTHON_UDF);
  if (NULL == info) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate memory failed", K(ret));
  } else {
    OZ(info->from_raw_expr(fun_sys));
    rt_expr.extra_info_ = info;
  }
  //绑定eval
  rt_expr.eval_func_ = ObExprPythonUdf::eval_test_udf;
  //绑定向量化eval
  bool is_batch = true;
  for(int i = 0; i < rt_expr.arg_cnt_; i++){
    if(!rt_expr.args_[i]->is_batch_result() && !rt_expr.args_[i]->is_const_expr()) {
      is_batch = false;
      break;
    }
  }
  if(is_batch) {
    rt_expr.eval_batch_func_ = ObExprPythonUdf::eval_test_udf_batch;
    //rt_expr.eval_batch_func_ = ObExprPythonUdf::eval_python_udf_batch_pyobject;
  } else {
    rt_expr.extra_buf_.buf_flag_ = false;
  }
  return ret;
}

//异常输出
void ObExprPythonUdf::message_error_dialog_show(char* buf) {
  const char* folder_dir = "/home/obtest/log/";
  if (access(folder_dir, 0) == -1) {
    // 不存在该文件夹则创建
    mkdir(folder_dir, S_IRWXU);
  } else {
    // 存在该文件夹则写入异常信息
    std::ofstream ofile;
    if (ofile) {
      ofile.open("/home/obtest/log/python_error", std::ios::out);
      ofile << buf;
      ofile.close();
    }
  }
  return;
}
//异常处理
void ObExprPythonUdf::process_python_exception() {
  char buf[65536], *buf_p = buf;
  PyObject *type_obj, *value_obj, *traceback_obj;
  PyErr_Fetch(&type_obj, &value_obj, &traceback_obj);
  if (value_obj == NULL)
      return;
  PyObject *pstr = PyObject_Str(value_obj);
  const char* value = PyUnicode_AsUTF8(pstr);
  size_t szbuf = sizeof(buf);
  int l;
  PyCodeObject *codeobj;
  l = snprintf(buf_p, szbuf, ("Error Message:\n%s"), value);
  buf_p += l;
  szbuf -= l;
  if (traceback_obj != NULL) {
      l = snprintf(buf_p, szbuf, ("\n\nTraceback:\n"));
      buf_p += l;
      szbuf -= l;
      PyTracebackObject *traceback = (PyTracebackObject *)traceback_obj;
      for (; traceback && szbuf > 0; traceback = traceback->tb_next) {
          //codeobj = traceback->tb_frame->f_code;
          codeobj = PyFrame_GetCode(traceback->tb_frame);
          l = snprintf(buf_p, szbuf, "%s: %s(# %d)\n",
              PyUnicode_AsUTF8(PyObject_Str(codeobj->co_name)),
              PyUnicode_AsUTF8(PyObject_Str(codeobj->co_filename)),
              traceback->tb_lineno);
          buf_p += l;
          szbuf -= l;
      }
  }
  message_error_dialog_show(buf);
  //Py_XDECREF(type_obj);
  //Py_XDECREF(value_obj);
  //Py_XDECREF(traceback_obj);
}

int ObPythonUdfInfo::deep_copy(common::ObIAllocator &allocator,
                                const ObExprOperatorType type,
                                ObIExprExtraInfo *&copied_info) const
{
  int ret = common::OB_SUCCESS;
  OZ(ObExprExtraInfoFactory::alloc(allocator, type, copied_info));
  ObPythonUdfInfo &other = *static_cast<ObPythonUdfInfo *>(copied_info);
  OZ(ObExprPythonUdf::deep_copy_udf_meta(other.udf_meta_, allocator, udf_meta_));
  return ret;
}

int ObPythonUdfInfo::from_raw_expr(const ObPythonUdfRawExpr &raw_expr)
{
  int ret = OB_SUCCESS;
  OZ(ObExprPythonUdf::deep_copy_udf_meta(udf_meta_, allocator_, raw_expr.get_udf_meta()));
  //predict_size = 4096; // max
  predict_size = 256; //default
  tps_s = 0; // init
  round = 0; // init
  lambda = 0.1;
  alpha = 0.25;
  delta = 256;
  round_limit = 10;
  return ret;
}

OB_DEF_SERIALIZE(ObPythonUdfInfo)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_ENCODE,
              udf_meta_);
  return ret;
}

OB_DEF_DESERIALIZE(ObPythonUdfInfo)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_DECODE,
              udf_meta_);
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObPythonUdfInfo)
{
  int64_t len = 0;
  LST_DO_CODE(OB_UNIS_ADD_LEN,
              udf_meta_);
  return len;
}
}  // namespace sql
}  // namespace oceanbase
