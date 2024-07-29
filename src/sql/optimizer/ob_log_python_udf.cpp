#define USING_LOG_PREFIX SQL_OPT
#include "sql/optimizer/ob_log_python_udf.h"
#include "sql/optimizer/ob_log_plan.h"
#include "sql/optimizer/ob_opt_est_cost.h"
#include "sql/optimizer/ob_join_order.h"
#include "common/ob_smart_call.h"
using namespace oceanbase::sql;
using namespace oceanbase::common;

int ObLogPythonUDF::get_op_exprs(ObIArray<ObRawExpr*> &all_exprs) {
  int ret = OB_SUCCESS;
  if (OB_FAIL(append(all_exprs, get_python_udf_exprs()))) {
    LOG_WARN("get python udf exprs failed", K(ret));
  } else if (OB_FAIL(ObLogicalOperator::get_op_exprs(all_exprs))) {
    LOG_WARN("get op exprs failed", K(ret));
  } else { /*do noting*/ }
  return ret;
}

int ObLogPythonUDF::allocate_expr_post(ObAllocExprContext &ctx)
{
  int ret = OB_SUCCESS;
  // print logs
  for (int64_t i = 0; OB_SUCC(ret) && i < output_exprs_.count(); i++) {
    ObRawExpr *expr = NULL;
    expr = output_exprs_.at(i);
  }
  ObLogicalOperator *op = child_.at(0);
  if (op != NULL) {
    for (int64_t i = 0; OB_SUCC(ret) && i < op->get_output_exprs().count(); i++) {
      ObRawExpr *expr = NULL;
      expr = op->get_output_exprs().at(i);
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < python_udf_exprs_.count(); i++) {
    ObRawExpr *expr = NULL;
    if (OB_ISNULL(expr = python_udf_exprs_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (OB_FAIL(mark_expr_produced(expr, branch_id_, id_, ctx))) {
      LOG_WARN("failed to mark expr as produced", K(ret));
    } else if (!is_plan_root() && OB_FAIL(output_exprs_.push_back(expr))) {
      LOG_WARN("failed to push back expr", K(ret));
    } else { /*do nothing*/ }
  }
  // check if we can produce some more exprs, such as 1 + 'c1' after we have produced 'c1'
  if(OB_SUCC(ret)) {
    if (OB_FAIL(ObLogicalOperator::allocate_expr_post(ctx))) {
      LOG_WARN("failed to allocate expr pre", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObLogPythonUDF::is_my_fixed_expr(const ObRawExpr *expr, bool &is_fixed)
{
  int ret = OB_SUCCESS;
  is_fixed = false;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret));
  } else {
    is_fixed = (T_FUN_PYTHON_UDF == expr->get_expr_type() && ObOptimizerUtil::find_item(python_udf_exprs_, expr));
  }
  return ret;
}

int ObLogPythonUDF::get_plan_item_info(PlanText &plan_text,
                                       ObSqlPlanItem &plan_item)
{
  int ret = OB_SUCCESS;
  // print subplan operator info
  if (OB_FAIL(ObLogicalOperator::get_plan_item_info(plan_text, plan_item))) {
    LOG_WARN("failed to get base plan item info", K(ret));
  } else if (OB_SUCC(ret)) {
    BEGIN_BUF_PRINT;
    for (int64_t i = 0; i < python_udf_exprs_.count() && OB_SUCC(ret); ++i) {
      ObPythonUdfRawExpr *python_udf_expr = python_udf_exprs_.at(i);
      const ObPythonUDFMeta &meta = python_udf_expr->get_udf_meta();
      if (i != 0 && OB_FAIL(BUF_PRINTF("\n      "))) {
        LOG_WARN("BUF_PRINTF fails", K(ret));
      } else {
        EXPLAIN_PRINT_EXPR(python_udf_expr, type);
      }
      // python_udf_meta_data
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(BUF_PRINTF(", "))) {
        LOG_WARN("BUF_PRINTF fails", K(ret));
      } else if (OB_FAIL(BUF_PRINTF("name("))) {
      } else if (OB_FAIL(BUF_PRINTF("%.*s", meta.name_.length(), meta.name_.ptr()))) {
      } else if (OB_FAIL(BUF_PRINTF(")"))) {
      }
    }
    END_BUF_PRINT(plan_item.special_predicates_,
                  plan_item.special_predicates_len_);
  }
  return ret;
}

int ObLogPythonUDF::collect_expr_metadata(
    ObRawExpr *expr,
    ObIArray<ObPythonUDFMeta> &metas)
{
  int ret = OB_SUCCESS;
  if(expr->get_expr_type() == T_FUN_PYTHON_UDF) {
    OZ(metas.push_back(static_cast<ObPythonUdfRawExpr *>(expr)->get_udf_meta()));
  }
  for (int32_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); i++) {
    OZ(collect_expr_metadata(expr->get_param_expr(i), metas));
  }
  return ret;
}
