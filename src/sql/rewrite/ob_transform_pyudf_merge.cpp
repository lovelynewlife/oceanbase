/**
 * author: xyyang
 * find redundancy of python udf and then remove them
 */
#define USING_LOG_PREFIX SQL_REWRITE
#include <regex>
#include <string>
#include <map>
#include "sql/rewrite/ob_transform_pyudf_merge.h"
#include "sql/rewrite/ob_stmt_comparer.h"
#include "sql/rewrite/ob_transform_utils.h"
#include "sql/optimizer/ob_optimizer_util.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/rewrite/ob_predicate_deduce.h"
#include "share/schema/ob_table_schema.h"
#include "common/ob_smart_call.h"
#include "sql/rewrite/onnx_optimizer/onnxoptimizer/optimize_c_api/optimize_c_api.h"
// #include "sql/engine/expr/ob_expr_python_udf.h"

#include "objit/include/objit/expr/ob_iraw_expr.h"
#include "sql/resolver/expr/ob_raw_expr.h"

#include "sql/ob_select_stmt_printer.h"
#include "deps/oblib/src/lib/json/ob_json_print_utils.h"

using namespace oceanbase::sql;
using namespace oceanbase::common;

string opted_model_path="/root/onnx_output/model_opted.onnx";
std::map<ObItemType, string> predicate_map={
  {T_OP_OR,"Or"},
  {T_OP_AND,"And"},
  {T_OP_GT,"Greater"},
  {T_OP_GE,"GreaterOrEqual"},
  {T_OP_LT,"Less"},
  {T_OP_LE,"LessOrEqual"},
  {T_OP_EQ,"Equal"},
  {T_OP_ADD,"Add"},
  {T_OP_MUL,"Mul"}
};
std::map<ObItemType, ObItemType> predicate_opposite_map={
  {T_OP_OR,T_OP_OR},
  {T_OP_AND,T_OP_AND},
  {T_OP_GT,T_OP_LT},
  {T_OP_GE,T_OP_LE},
  {T_OP_LT,T_OP_GT},
  {T_OP_LE,T_OP_GE},
  {T_OP_EQ,T_OP_EQ},
  {T_OP_ADD,T_OP_ADD},
  {T_OP_MUL,T_OP_MUL}
};

ObTransformPyUDFMerge::ObTransformPyUDFMerge(ObTransformerCtx *ctx)
    : ObTransformRule(ctx, TransMethod::POST_ORDER, T_PYUDF_MERGE),
      allocator_("PyUDFMerge")
{}

ObTransformPyUDFMerge::~ObTransformPyUDFMerge()
{}

// 没有考虑不同的python udf的输入不同的情况，默认所有udf输入都相同，只有推理的模型不同，并且所有udf的输入是一样的
int ObTransformPyUDFMerge::transform_one_stmt(
  common::ObIArray<ObParentDMLStmt> &parent_stmts, ObDMLStmt *&stmt, bool &trans_happened)
{
  int ret = OB_SUCCESS;
  trans_happened = false;
  LOG_TRACE("Run transform ObTransformPyUDFMerge", K(ret));
  ObSelectStmt *select_stmt = NULL;
  string onnx_model_opted_path;
  if (OB_ISNULL(stmt) || OB_ISNULL(ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("stmt is null", K(ret), K(stmt), K(ctx_));
  } else if (!stmt->is_select_stmt()) {
    // do nothing
    OPT_TRACE("not select stmt, can not transform");
  } else if (FALSE_IT(select_stmt = static_cast<ObSelectStmt*>(stmt))) {
    //准备进行改写
    LOG_WARN("select stmt is NULL", K(ret));
  } else if (select_stmt->get_condition_exprs().empty()) {
    //没有改写空间
    LOG_WARN("input preds is empty", K(ret));
  } 
  else if(OB_FAIL(merge_python_udf_expr_in_condition(select_stmt->get_condition_exprs(), onnx_model_opted_path))){
    LOG_WARN("merge python udf in condition fail", K(ret));
  } else if(OB_FAIL(push_predicate_into_onnx_model(select_stmt->get_condition_exprs(), onnx_model_opted_path))){
    LOG_WARN("merge python udf in condition fail", K(ret));
  } else if(optimize_on_merged_onnx_model(onnx_model_opted_path)){
    LOG_WARN("optimize_on_merged_model fail", K(ret));
  }
  else{
    trans_happened = true;
    stmt = select_stmt;
  }
  return ret;  

}

int ObTransformPyUDFMerge::optimize_on_merged_onnx_model(string& out_path)
{
  int ret = OB_SUCCESS;
  try{
        optimize_on_merged_model(out_path,out_path);
      } catch(...){
        LOG_WARN("optimize_with_model_path fail");
        ret=OB_ERROR;
      }
  return ret;
}

int ObTransformPyUDFMerge::push_predicate_into_onnx_model(
  ObIArray<ObRawExpr *> &src_exprs,
  string& out_path)
{
  int ret = OB_SUCCESS;
  // 辅助获取model前缀
  std::map<string,int> countMap;

  // 遍历表达式树，找到所有包含python udf的表达式树的根节点(maybe只有and连接的情况)
  ObSEArray<ObRawExpr *, 4> exprs_contain_python_udf_list;
  for(int i=0;i<src_exprs.count();i++){
    if(ObTransformUtils::expr_contain_type(src_exprs.at(i), T_FUN_SYS_PYTHON_UDF)) {
        exprs_contain_python_udf_list.push_back(src_exprs.at(i));
        src_exprs.remove(i);
        i--;
      }
  }

  // 对每个包含python udf的表达式树的根节点进行修改
  int udf_input_count;
  std::vector<string> prefix_list;
  ObSEArray<ObRawExpr *, 4> expr_opted_list;
  for(int i=0; i<exprs_contain_python_udf_list.count(); i++){
    ObRawExpr * expr=exprs_contain_python_udf_list.at(i);
    int64_t child_count=expr->get_param_count();
    if(child_count!=2){
      LOG_WARN("child_count is not two");
      return OB_ERROR;
    }else{
      ObRawExpr* expr_opted=nullptr;
      int level_count;
      string prefix;
      if (OB_FAIL(ObTransformPyUDFMerge::push_predicate_down(prefix, expr, out_path, countMap, expr_opted, udf_input_count, level_count))) {
        LOG_WARN("failed to push_predicate_down at top", K(ret));
      }
      prefix_list.push_back(prefix);
      expr_opted_list.push_back(expr_opted);
    }
  }

  // 将包含python udf的表达式树的根节点合并成一个expr
  string predicate="And";
  while(expr_opted_list.count()!=1){
    ObRawExpr* expr_opted_l=expr_opted_list.at(0);
    ObRawExpr* expr_opted_r=expr_opted_list.at(1);
    string prefix_l=prefix_list.at(0);
    string prefix_r=prefix_list.at(1);
    try{
      merge_double_models_with_predicate(out_path,predicate,prefix_l,prefix_r);
    } catch(...){
      LOG_WARN("merge_double_models_with_predicate fail");
      ret=OB_ERROR;
    }
    ObPythonUdfRawExpr* python_udf_expr_opted_l=static_cast<ObPythonUdfRawExpr*>(expr_opted_l);
    ObPythonUdfRawExpr* python_udf_expr_opted_r=static_cast<ObPythonUdfRawExpr*>(expr_opted_r);
    oceanbase::share::schema::ObPythonUDFMeta udf_meta_opted_l=python_udf_expr_opted_l->get_udf_meta();
    oceanbase::share::schema::ObPythonUDFMeta udf_meta_opted_r=python_udf_expr_opted_r->get_udf_meta();
    // 生成新的prefix
    string prefix=prefix_l+"_"+prefix_r;
    prefix_list.erase(prefix_list.begin());
    prefix_list.erase(prefix_list.begin());
    prefix_list.push_back(prefix);
    // 生成新的udf_meta
    oceanbase::share::schema::ObPythonUDFMeta udf_meta_opted;
    udf_meta_opted.name_="udf_opted";
    udf_meta_opted.pycall_=udf_meta_opted_l.pycall_;
    // todo 现在默认只返回bool值,并且所有udf的输入值全相同
    udf_meta_opted.ret_=ObPythonUDF::PyUdfRetType::INTEGER;
    udf_meta_opted.udf_attributes_types_=udf_meta_opted_l.udf_attributes_types_;
    for(int i=udf_input_count;i<udf_meta_opted_r.udf_attributes_types_.count();i++){
      udf_meta_opted.udf_attributes_types_.push_back(udf_meta_opted_r.udf_attributes_types_.at(i));
    }
    // 构造新的python_udf_expr
    int64_t param_count_l = python_udf_expr_opted_l->get_param_count();
    int64_t param_count_r = python_udf_expr_opted_r->get_param_count();
    ObRawExpr* expr_opted;
    if (OB_ISNULL(ctx_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("transform context is null", K(ret), K(ctx_));
    } else if (OB_FAIL(ObRawExprCopier::copy_expr_node(*(*ctx_).expr_factory_,
                                                  expr_opted_l,
                                                  expr_opted))) {
      LOG_WARN("failed to copy the predicate", K(ret));
    }else{
      ObPythonUdfRawExpr* python_udf_expr_opted=static_cast<ObPythonUdfRawExpr*>(expr_opted);
      for(int i=udf_input_count; i<param_count_r; i++){
        python_udf_expr_opted->add_param_expr(python_udf_expr_opted_r->get_param_expr(i));
      }
      python_udf_expr_opted->set_udf_meta(udf_meta_opted);
      expr_opted=python_udf_expr_opted;
      expr_opted_list.remove(0);
      expr_opted_list.remove(0);
      expr_opted_list.push_back(expr_opted);
    }
  }
  ObRawExpr* expr=expr_opted_list.at(0);
  if (OB_FAIL(expr->formalize(ctx_->session_info_))) {
        LOG_WARN("failed to formalize", K(ret));
  }
  // 构建bool expr
  ObRawExpr *bool_expr = NULL;
  ObRawExpr *equal_expr = NULL;
  if (OB_FAIL(ObRawExprUtils::build_const_bool_expr(ctx_->expr_factory_, bool_expr, true))) {
          LOG_WARN("failed to build const bool expr", K(ret));
  }else if (OB_FAIL(ObRawExprUtils::create_equal_expr(*(ctx_->expr_factory_),
                                                        ctx_->session_info_,
                                                        bool_expr,
                                                        expr,
                                                        equal_expr))) {
      LOG_WARN("Creation of equal expr for expr_opted fails", K(ret));
  }
  src_exprs.push_back(equal_expr);
  return ret;
}

int ObTransformPyUDFMerge::push_predicate_down(string& prefix, ObRawExpr * src_expr, string& out_path, std::map<string,int>& countMap, ObRawExpr*& expr_opted, int& udf_input_count, int& level_count){
  int ret = OB_SUCCESS;
  // 终止条件
  if(src_expr->get_expr_type() == T_FUN_SYS_PYTHON_UDF){
    ObPythonUdfRawExpr* python_udf_expr;
    if (FALSE_IT(python_udf_expr= static_cast<ObPythonUdfRawExpr*>(src_expr))) {
      LOG_WARN("convert expr to ObPythonUdfRawExpr fail", K(ret));
    }else{
      oceanbase::share::schema::ObPythonUDFMeta udf_meta_opted;
      // 替换model地址
      oceanbase::share::schema::ObPythonUDFMeta udf_meta=python_udf_expr->get_udf_meta();
      ObString pycall_ob=udf_meta.pycall_;
      string pycall(pycall_ob.ptr(),pycall_ob.length());
      std::regex pattern("onnx_path='(.*?)'");
      string exchanged_onnx_path="onnx_path='"+out_path+"'";
      string result = std::regex_replace(pycall, pattern, exchanged_onnx_path);
      udf_meta_opted.pycall_=ObString(result.c_str());
      LOG_TRACE("change model path in pycall sucess", K(ret));

      // 获取udf前缀名
      ObString name=udf_meta.name_;
      string udfname(name.ptr(),name.length());
      std::map<string,int>::iterator it=countMap.find(udfname);
      if(it==countMap.end()){
        countMap[udfname]=1;
      }else{
        int value=countMap[udfname];
        countMap[udfname]=value+1;
      }
      string num=std::to_string(countMap[udfname]);
      prefix=udfname+"_"+num;
      // 构建udf meta
      udf_meta_opted.name_="udf_opted";
      udf_meta_opted.udf_attributes_types_=udf_meta.udf_attributes_types_;
      udf_meta_opted.ret_=udf_meta.ret_;
      // 构建expr
      if (OB_ISNULL(ctx_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("transform context is null", K(ret), K(ctx_));
      } else if (OB_FAIL(ObRawExprCopier::copy_expr_node(*(*ctx_).expr_factory_,
                                                  src_expr,
                                                  expr_opted))) {
        LOG_WARN("failed to copy the predicate", K(ret));
      }else{
        ObPythonUdfRawExpr* python_udf_expr_opted=static_cast<ObPythonUdfRawExpr*>(expr_opted);
        python_udf_expr_opted->set_udf_meta(udf_meta_opted);
        expr_opted=python_udf_expr_opted;
        udf_input_count=udf_meta.udf_attributes_types_.count();
        level_count=1;
      }
    }
    return ret;
  }

  ObRawExpr * expr_l=src_expr->get_param_expr(0);
  ObRawExpr * expr_r=src_expr->get_param_expr(1);
  int count_l=ObTransformUtils::count_python_udf_num(const_cast<ObRawExpr *>(expr_l));
  int count_r=ObTransformUtils::count_python_udf_num(const_cast<ObRawExpr *>(expr_r));
  // 左右子树都有python udf
  if(count_l>0&&count_r>0){
    string prefix_l="";
    string prefix_r="";
    ObRawExpr* expr_opted_l=nullptr;
    ObRawExpr* expr_opted_r=nullptr;
    int udf_input_count_l=0;
    int udf_input_count_r=0;
    // 遍历左子树
    if(OB_FAIL(push_predicate_down(prefix_l, expr_l, out_path, countMap, expr_opted_l, udf_input_count_l, level_count))){
      LOG_WARN("push_predicate_down fail", K(ret));
    } 
    // 遍历右子树
    if(OB_FAIL(push_predicate_down(prefix_r, expr_r, out_path, countMap, expr_opted_r, udf_input_count_r, level_count))){
      LOG_WARN("push_predicate_down fail", K(ret));
    }
    // 谓词下推到onnx模型中
    ObItemType type=src_expr->get_expr_type();
    string predicate=predicate_map[type];
    try{
      merge_double_models_with_predicate(out_path,predicate,prefix_l,prefix_r);
    } catch(...){
      LOG_WARN("merge_double_models_with_predicate fail");
      ret=OB_ERROR;
    }
    ObPythonUdfRawExpr* python_udf_expr_opted_l=static_cast<ObPythonUdfRawExpr*>(expr_opted_l);
    ObPythonUdfRawExpr* python_udf_expr_opted_r=static_cast<ObPythonUdfRawExpr*>(expr_opted_r);
    oceanbase::share::schema::ObPythonUDFMeta udf_meta_opted_l=python_udf_expr_opted_l->get_udf_meta();
    oceanbase::share::schema::ObPythonUDFMeta udf_meta_opted_r=python_udf_expr_opted_r->get_udf_meta();
    // 生成新的prefix
    prefix=prefix_l+"_"+prefix_r;
    // 生成新的udf_meta
    oceanbase::share::schema::ObPythonUDFMeta udf_meta_opted;
    udf_meta_opted.name_="udf_opted";
    udf_meta_opted.pycall_=udf_meta_opted_l.pycall_;
    // todo 现在默认只返回bool值,并且所有udf的输入值全相同
    udf_meta_opted.ret_=ObPythonUDF::PyUdfRetType::INTEGER;
    udf_meta_opted.udf_attributes_types_=udf_meta_opted_l.udf_attributes_types_;
    for(int i=udf_input_count_l;i<udf_meta_opted_r.udf_attributes_types_.count();i++){
      udf_meta_opted.udf_attributes_types_.push_back(udf_meta_opted_r.udf_attributes_types_.at(i));
    }
    // 构造新的python_udf_expr
    int64_t param_count_l = python_udf_expr_opted_l->get_param_count();
    int64_t param_count_r = python_udf_expr_opted_r->get_param_count();
    if (OB_ISNULL(ctx_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("transform context is null", K(ret), K(ctx_));
    } else if (OB_FAIL(ObRawExprCopier::copy_expr_node(*(*ctx_).expr_factory_,
                                                  expr_opted_l,
                                                  expr_opted))) {
      LOG_WARN("failed to copy the predicate", K(ret));
    }else{
      ObPythonUdfRawExpr* python_udf_expr_opted=static_cast<ObPythonUdfRawExpr*>(expr_opted);
      for(int i=udf_input_count_l; i<param_count_r; i++){
        python_udf_expr_opted->add_param_expr(python_udf_expr_opted_r->get_param_expr(i));
      }
      python_udf_expr_opted->set_udf_meta(udf_meta_opted);
      expr_opted=python_udf_expr_opted;
      udf_input_count=udf_input_count_l;
      level_count=1;
    }
  } 

  // 左子树有，右子树没有
  else if(count_l>0&&count_r==0){
    string prefix_l="";
    ObRawExpr* expr_opted_l=nullptr;
    int udf_input_count_l=0;
    // 遍历左子树
    if(OB_FAIL(push_predicate_down(prefix_l, expr_l, out_path, countMap, expr_opted_l, udf_input_count_l, level_count))){
      LOG_WARN("push_predicate_down fail", K(ret));
    } 
    // 获取右子树的返回值类型
    ObObjType dataType=expr_r->get_data_type();
    string inputType;
    ObPythonUDF::PyUdfRetType udf_meta_ret_type;
    switch(dataType) {
        case ObCharType :
        case ObVarcharType :
        case ObTinyTextType :
        case ObTextType :
        case ObMediumTextType :
        case ObLongTextType : {
          inputType="string";
          udf_meta_ret_type=ObPythonUDF::PyUdfRetType::STRING;
          break;
        }
        case ObTinyIntType :
        case ObSmallIntType :
        case ObMediumIntType :
        case ObInt32Type :
        case ObIntType :{
          inputType="int";
          udf_meta_ret_type=ObPythonUDF::PyUdfRetType::INTEGER;
          break;
        }
        case ObDoubleType :{
          inputType="double";
          udf_meta_ret_type=ObPythonUDF::PyUdfRetType::REAL;
          break;
        }
        // case ObNumberType :{
        //   inputType="double";
        //   udf_meta_ret_type=ObPythonUDF::PyUdfRetType::DECIMAL;
        //   break;
        // }
        default : 
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("not support param type in rewrite", K(ret));
      }
      // 谓词下推到onnx模型中
      ObItemType type=src_expr->get_expr_type();
      string predicate=predicate_map[type];
      try{
        merge_single_model_with_predicate(out_path,predicate,inputType,prefix_l,level_count);
      } catch(...){
        LOG_WARN("merge_double_models_with_predicate fail");
        ret=OB_ERROR;
      }
      ObPythonUdfRawExpr* python_udf_expr_opted_l=static_cast<ObPythonUdfRawExpr*>(expr_opted_l);
      oceanbase::share::schema::ObPythonUDFMeta udf_meta_opted_l=python_udf_expr_opted_l->get_udf_meta();
      // 生成新的prefix
      prefix=prefix_l;
      // 生成新的udf_meta
      oceanbase::share::schema::ObPythonUDFMeta udf_meta_opted;
      udf_meta_opted.name_="udf_opted";
      udf_meta_opted.pycall_=udf_meta_opted_l.pycall_;
      // todo 现在默认只返回bool值,并且所有udf的输入值全相同
      udf_meta_opted.ret_=ObPythonUDF::PyUdfRetType::INTEGER;
      udf_meta_opted.udf_attributes_types_=udf_meta_opted_l.udf_attributes_types_;
      udf_meta_opted.udf_attributes_types_.push_back(udf_meta_ret_type);
      // 构造新的python_udf_expr
      if (OB_ISNULL(ctx_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("transform context is null", K(ret), K(ctx_));
      } else if (OB_FAIL(ObRawExprCopier::copy_expr_node(*(*ctx_).expr_factory_,
                                                  expr_opted_l,
                                                  expr_opted))) {
        LOG_WARN("failed to copy the predicate", K(ret));
      }else{
        ObPythonUdfRawExpr* python_udf_expr_opted=static_cast<ObPythonUdfRawExpr*>(expr_opted);
        python_udf_expr_opted->add_param_expr(expr_r);
        python_udf_expr_opted->set_udf_meta(udf_meta_opted);
        expr_opted=python_udf_expr_opted;
        udf_input_count=udf_input_count_l;
        level_count=level_count+1;
      }

  }

  // 右子树有，左子树没有
  else if(count_l==0&&count_r>0){
    string prefix_r="";
    ObRawExpr* expr_opted_r=nullptr;
    int udf_input_count_r=0;
    // 遍历右子树
    if(OB_FAIL(push_predicate_down(prefix_r, expr_r, out_path, countMap, expr_opted_r, udf_input_count_r, level_count))){
      LOG_WARN("push_predicate_down fail", K(ret));
    } 
    // 获取左子树的返回值类型
    ObObjType dataType=expr_l->get_data_type();
    string inputType;
    ObPythonUDF::PyUdfRetType udf_meta_ret_type;
    switch(dataType) {
        case ObCharType :
        case ObVarcharType :
        case ObTinyTextType :
        case ObTextType :
        case ObMediumTextType :
        case ObLongTextType : {
          inputType="string";
          udf_meta_ret_type=ObPythonUDF::PyUdfRetType::STRING;
          break;
        }
        case ObTinyIntType :
        case ObSmallIntType :
        case ObMediumIntType :
        case ObInt32Type :
        case ObIntType :{
          inputType="int";
          udf_meta_ret_type=ObPythonUDF::PyUdfRetType::INTEGER;
          break;
        }
        case ObDoubleType :{
          inputType="double";
          udf_meta_ret_type=ObPythonUDF::PyUdfRetType::REAL;
          break;
        }
        // case ObNumberType :{
        //   inputType="double";
        //   udf_meta_ret_type=ObPythonUDF::PyUdfRetType::DECIMAL;
        //   break;
        // }
        default : 
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("not support param type in rewrite", K(ret));
      }
      // 谓词下推到onnx模型中
      ObItemType type=src_expr->get_expr_type();
      string predicate=predicate_map[predicate_opposite_map[type]];
      try{
        merge_single_model_with_predicate(out_path,predicate,inputType,prefix_r,level_count);
      } catch(...){
        LOG_WARN("merge_double_models_with_predicate fail");
        ret=OB_ERROR;
      }
      ObPythonUdfRawExpr* python_udf_expr_opted_r=static_cast<ObPythonUdfRawExpr*>(expr_opted_r);
      oceanbase::share::schema::ObPythonUDFMeta udf_meta_opted_r=python_udf_expr_opted_r->get_udf_meta();
      // 生成新的prefix
      prefix=prefix_r;
      // 生成新的udf_meta
      oceanbase::share::schema::ObPythonUDFMeta udf_meta_opted;
      udf_meta_opted.name_="udf_opted";
      udf_meta_opted.pycall_=udf_meta_opted_r.pycall_;
      // todo 现在默认只返回bool值,并且所有udf的输入值全相同
      udf_meta_opted.ret_=ObPythonUDF::PyUdfRetType::INTEGER;
      udf_meta_opted.udf_attributes_types_=udf_meta_opted_r.udf_attributes_types_;
      udf_meta_opted.udf_attributes_types_.push_back(udf_meta_ret_type);
      // 构造新的python_udf_expr
      if (OB_ISNULL(ctx_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("transform context is null", K(ret), K(ctx_));
      } else if (OB_FAIL(ObRawExprCopier::copy_expr_node(*(*ctx_).expr_factory_,
                                                  expr_opted_r,
                                                  expr_opted))) {
        LOG_WARN("failed to copy the predicate", K(ret));
      }else{
        ObPythonUdfRawExpr* python_udf_expr_opted=static_cast<ObPythonUdfRawExpr*>(expr_opted);
        python_udf_expr_opted->add_param_expr(expr_l);
        python_udf_expr_opted->set_udf_meta(udf_meta_opted);
        expr_opted=python_udf_expr_opted;
        udf_input_count=udf_input_count_r;
        level_count=level_count+1;
      }
  }

  return ret;
}

int ObTransformPyUDFMerge::merge_python_udf_expr_in_condition(
  ObIArray<ObRawExpr *> &src_exprs,
  string& out_path)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObPythonUdfRawExpr *, 4> python_udf_expr_list;
  out_path=opted_model_path;
  if(OB_FAIL(extract_python_udf_expr_in_condition(python_udf_expr_list, src_exprs))){
    LOG_WARN("extract_python_udf_expr_in_condition fail", K(ret));
  } else if(OB_FAIL(merge_onnx_model_from_python_udf_expr_list(out_path, python_udf_expr_list))){
    LOG_WARN("merge_onnx_model_from_python_udf_expr_list fail", K(ret));
  }
  return ret;
}

int ObTransformPyUDFMerge::extract_python_udf_expr_in_condition(
  ObIArray<ObPythonUdfRawExpr *> &python_udf_expr_list,
  ObIArray<ObRawExpr *> &src_exprs)
{
  int ret = OB_SUCCESS;
  for(int i=0;i<src_exprs.count();i++){
    if(OB_FAIL(ObTransformUtils::extract_all_python_udf_raw_expr_in_raw_expr(python_udf_expr_list,src_exprs.at(i)))){
      LOG_WARN("extract_all_python_udf_raw_expr_in_raw_expr fail", K(ret));
    }
  }
  return ret;
}

int ObTransformPyUDFMerge::merge_onnx_model_from_python_udf_expr_list(
  string& out_path, 
  ObIArray<ObPythonUdfRawExpr *> &python_udf_expr_list){

  int ret = OB_SUCCESS;
  int count=1;
  std::map<string,int> countMap;
  std::vector<string> prefix_list;
  std::vector<string> model_path_list;

  for(int i=0;i<python_udf_expr_list.count();i++){
    oceanbase::share::schema::ObPythonUDFMeta meta=python_udf_expr_list.at(i)->get_udf_meta();
    LOG_TRACE("onnx udf name is ", K(meta.name_));
    ObString name=meta.name_;
    string udfname(name.ptr(),name.length());
    // 确定model前缀 “udfname_num_”
    std::map<string,int>::iterator it=countMap.find(udfname);
    if(it==countMap.end()){
      countMap[udfname]=1;
    }else{
      int value=countMap[udfname];
      countMap[udfname]=value+1;
    }
    string num=std::to_string(countMap[udfname]);
    string prefix=udfname+"_"+num+"_";
    prefix_list.push_back(prefix);

    
    LOG_TRACE("onnx model prefix is ", K(ObString(prefix.c_str())));
    // 获取model path
    string model_path;
    if(OB_FAIL(get_onnx_model_path_from_python_udf_meta(model_path,meta))){
      LOG_WARN("get_onnx_model_path_from_python_udf_meta fail", K(ret));
    }
    model_path_list.push_back(model_path);
  }

  // 模型融合
  for(int i=0;i<model_path_list.size();i++){
    if(count==1){
      // 第一次取前两个model
      string path1=model_path_list.at(i);
      string pre1=prefix_list.at(i);
      i++;
      string path2=model_path_list.at(i);
      string pre2=prefix_list.at(i);
      count++;
      try{
        // optimize_with_model_path(path1,path2,pre1,pre2,out_path);
        merge_with_model_path(path1,path2,pre1,pre2,out_path);
      } catch(...){
        LOG_WARN("optimize_with_model_path fail");
        ret=OB_ERROR;
      }
    }else{
      string path1=out_path;
      string pre1="";
      string path2=model_path_list.at(i);
      string pre2=prefix_list.at(i);
      try{
        // optimize_with_model_path(path1,path2,pre1,pre2,out_path);
        merge_with_model_path(path1,path2,pre1,pre2,out_path);
      } catch(...){
        LOG_WARN("optimize_with_model_path fail");
        ret=OB_ERROR;
      }
    }
  }
  return ret;
}

int ObTransformPyUDFMerge::get_onnx_model_path_from_python_udf_meta(string &onnx_model_path, oceanbase::share::schema::ObPythonUDFMeta &python_udf_meta){
  int ret =OB_SUCCESS;
  // there is a bug in python
  // //pycall
  // std::string pycall(python_udf_meta.pycall_.ptr());
  // std::string pGetModelPath="\ndef pygetmodelpath():\
  // \n\treturn onnx_path\0";
  // pycall.append(pGetModelPath);

  // //runtime variables
  // const char* bytes=NULL;
  // PyObject *pModule = NULL;
  // PyObject *dic = NULL;
  // PyObject *v = NULL;
  // PyObject *pInitial = NULL;
  // PyObject *pGetModel = NULL;
  // PyObject *pResult = NULL;
  // PyObject* unicode = NULL;
  // const char* pycall_c = pycall.c_str();

  // // // 初始化 Python 解释器
  // // Py_Initialize();
  // //Acquire GIL
  // bool nStatus = PyGILState_Check();
  // PyGILState_STATE gstate;
  // if(!nStatus) {
  //   gstate = PyGILState_Ensure();
  //   nStatus = true;
  // }

  // // prepare and import python code
  // pModule = PyImport_AddModule("__main__"); // load main module
  // if(OB_ISNULL(pModule)) {
  //   ret = OB_ERR_UNEXPECTED;
  //   LOG_WARN("fail to import main module", K(ret));
  //   goto destruction;
  // }
  // dic = PyModule_GetDict(pModule); // get main module dic
  // if(OB_ISNULL(dic)) {
  //   ret = OB_ERR_UNEXPECTED;
  //   LOG_WARN("fail to get main module dic", K(ret));
  //   goto destruction;
  // } 
  // v = PyRun_StringFlags(pycall_c, Py_file_input, dic, dic, NULL); // test pycall
  // if(OB_ISNULL(v)) {
  //   ObExprPythonUdf::process_python_exception();
  //   ret = OB_ERR_UNEXPECTED;
  //   LOG_WARN("fail to write pycall into module", K(ret));
  //   goto destruction;
  // }
  // pInitial = PyObject_GetAttrString(pModule, "pyinitial"); // get pyInitial()
  // if(OB_ISNULL(pInitial) || !PyCallable_Check(pInitial)) {
  //   ObExprPythonUdf::process_python_exception();
  //   ret = OB_ERR_UNEXPECTED;
  //   LOG_WARN("Fail to import pyinitial", K(ret));
  //   goto destruction;
  // } else if (OB_ISNULL(PyObject_CallObject(pInitial, NULL))){
  //   ObExprPythonUdf::process_python_exception();
  //   ret = OB_ERR_UNEXPECTED;
  //   LOG_WARN("Fail to run pyinitial", K(ret));
  //   goto destruction;
  // } else {
  //   LOG_DEBUG("Import python udf pyinitial", K(ret));
  // }

  // pGetModel = PyObject_GetAttrString(pModule, "pygetmodelpath"); // get pygetmodelpath()
  // if(OB_ISNULL(pGetModel) || !PyCallable_Check(pGetModel)) {
  //   ObExprPythonUdf::process_python_exception();
  //   ret = OB_ERR_UNEXPECTED;
  //   LOG_WARN("Fail to import pGetModel", K(ret));
  //   goto destruction;
  // } else if (OB_ISNULL(PyObject_CallObject(pGetModel, NULL))){
  //   ObExprPythonUdf::process_python_exception();
  //   ret = OB_ERR_UNEXPECTED;
  //   LOG_WARN("Fail to run pGetModel", K(ret));
  //   goto destruction;
  // } else {
  //   LOG_DEBUG("Import python udf pGetModel", K(ret));
  // }
  // pResult=PyObject_CallObject(pGetModel, NULL);
  // if (pResult != NULL) {
  //       if (PyUnicode_Check(pResult)) { // 检查返回值是否是 Python 字符串
  //           unicode = PyUnicode_AsUTF8String(pResult); // 将 Python 字符串对象转换为 UTF-8 编码的字节对象
  //           if (unicode != NULL) {
  //               bytes = PyBytes_AsString(unicode); // 将字节对象转换为 C 字符串
  //               if (bytes != NULL) {
  //                   onnx_model_path = ObString(bytes); // 将 C 字符串转换为 C++ 字符串
  //               }
  //           }
  //       } else {
  //           LOG_WARN("Fail to get unicode", K(ret)); // 处理非字符串对象
  //       }
  //   } else {
  //       LOG_WARN("Fail to get pResult", K(ret)); // 处理函数调用失败
  //   }
  
  // destruction: 
  // // 释放 Python 对象
  // Py_XDECREF(dic);
  // Py_XDECREF(pModule);
  // Py_XDECREF(v);
  // Py_XDECREF(pInitial);
  // Py_XDECREF(pGetModel);
  // Py_XDECREF(pResult);
  // Py_XDECREF(unicode);
  // //release GIL
  // if(nStatus)
  //   PyGILState_Release(gstate);
  // LOG_DEBUG("get model path", K(onnx_model_path));
  // // // 关闭 Python 解释器
  // // Py_FinalizeEx();
  string pycall(python_udf_meta.pycall_.ptr());
  std::regex pattern("onnx_path='(.*?)'");
  std::smatch match;
  if (std::regex_search(pycall, match, pattern)) {
        if (match.size() > 1) {
          onnx_model_path=match[1].str();
          LOG_DEBUG("get model path", K(ObString(onnx_model_path.c_str())));
        } else {
          onnx_model_path=match[0].str();
          LOG_DEBUG("get model path", K(ObString(onnx_model_path.c_str())));
        }
    } else {
        LOG_DEBUG("get no model path");
    }
  return ret;
}

int ObTransformPyUDFMerge::need_transform(const common::ObIArray<ObParentDMLStmt> &parent_stmts,
  const int64_t current_level,
  const ObDMLStmt &stmt,
  bool &need_trans)
{
  int ret = OB_SUCCESS;
  LOG_TRACE("Check need transform of ObTransformPyUDFMerge.", K(ret));
  need_trans = false;
  ObSEArray<ObSelectStmt*, 16> child_stmts;
  int python_udf_count=0;

  for(int32_t i = 0; i < stmt.get_condition_size(); i++) {
    python_udf_count=python_udf_count+ObTransformUtils::count_python_udf_num(const_cast<ObRawExpr *>(stmt.get_condition_expr(i)));
  }

  if(python_udf_count>1){
    need_trans = true;
    LOG_DEBUG("this query need transform of ObTransformPyUDFMerge ,udf count is ",K(python_udf_count));
  }
  return ret;
}

int ObTransformPyUDFMerge::construct_transform_hint(ObDMLStmt &stmt, void *trans_params) 
{
  int ret = OB_SUCCESS;
  ObIArray<ObSelectStmt*> *transed_stmts = static_cast<ObIArray<ObSelectStmt*>*>(trans_params);
  if (OB_ISNULL(ctx_) || OB_ISNULL(ctx_->allocator_) || OB_ISNULL(transed_stmts)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(ctx_));
  } else if (OB_FAIL(ctx_->add_src_hash_val(ctx_->src_qb_name_))) {
    LOG_WARN("failed to add src hash val", K(ret));
  } else {
    ObTransHint *hint = NULL;
    ObString qb_name;
    ObSelectStmt *cur_stmt = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && i < transed_stmts->count(); i++) {
      if (OB_ISNULL(cur_stmt = transed_stmts->at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("transed_stmt is null", K(ret), K(i));
      } else if (OB_FAIL(ctx_->add_used_trans_hint(get_hint(cur_stmt->get_stmt_hint())))) {
        LOG_WARN("failed to add used hint", K(ret));
      } else if (OB_FAIL(ObQueryHint::create_hint(ctx_->allocator_, get_hint_type(), hint))) {
        LOG_WARN("failed to create hint", K(ret));
      } else if (OB_FAIL(ctx_->outline_trans_hints_.push_back(hint))) {
        LOG_WARN("failed to push back hint", K(ret));
      } else if (OB_FAIL(cur_stmt->get_qb_name(qb_name))) {
        LOG_WARN("failed to get qb name", K(ret));
      } else if (OB_FAIL(cur_stmt->adjust_qb_name(ctx_->allocator_,
                                                  qb_name,
                                                  ctx_->src_hash_val_))) {
        LOG_WARN("adjust stmt id failed", K(ret));
      } else {
        hint->set_qb_name(qb_name);
      }
    }
  }
  return ret;
}