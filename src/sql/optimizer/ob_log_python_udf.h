#ifndef _OB_LOG_PYTHON_UDF_H_
#define _OB_LOG_PYTHON_UDF_H_
#include "sql/optimizer/ob_logical_operator.h"
#include "share/schema/ob_python_udf.h"

namespace oceanbase
{
namespace sql
{
class ObLogPythonUDF : public ObLogicalOperator
{
public:
  ObLogPythonUDF(ObLogPlan &plan)
      : ObLogicalOperator(plan),
        python_udf_exprs_(),
        metas_()
  {}

  virtual ~ObLogPythonUDF() {};

  int generate_input_exprs();
  inline int add_python_udf_expr(ObPythonUdfRawExpr *python_udf_expr)
  { return python_udf_exprs_.push_back(python_udf_expr); }
  inline ObIArray<ObPythonUdfRawExpr *> &get_python_udf_exprs() { return python_udf_exprs_; }
  inline const ObIArray<ObPythonUdfRawExpr *> &get_python_udf_exprs() const { return python_udf_exprs_; }


  // show python udfs in explain 
  virtual int get_plan_item_info(PlanText &plan_text,
                                ObSqlPlanItem &plan_item) override;
                                
  virtual int collect_expr_metadata(ObRawExpr *expr,
                                    ObIArray<ObPythonUDFMeta> &metas);

  virtual int get_op_exprs(ObIArray<ObRawExpr*> &all_exprs) override;

  virtual int allocate_expr_post(ObAllocExprContext &ctx) override;
  
  virtual int is_my_fixed_expr(const ObRawExpr *expr, bool &is_fixed) override;
  
private:
  ObSEArray<ObPythonUdfRawExpr *, 4, common::ModulePageAllocator, true> python_udf_exprs_;
  ObSEArray<ObPythonUDFMeta, 4, common::ModulePageAllocator, true> metas_;
  DISALLOW_COPY_AND_ASSIGN(ObLogPythonUDF);
};

} // end of namespace sql
} // end of namespace oceanbase


#endif /* OB_LOG_SUBQUERY_SCAN_H_ */
