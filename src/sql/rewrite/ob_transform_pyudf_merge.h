/**
 * author: xyyang
 * find redundancy of python udf and then remove them
 */
#ifndef OB_TRANSFORM_PYUDF_MERGE_H
#define OB_TRANSFORM_PYUDF_MERGE_H

#include "sql/rewrite/ob_transform_rule.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/ob_select_stmt_printer.h"

namespace oceanbase
{
namespace sql
{

class ObTransformPyUDFMerge : public ObTransformRule
{
public:
  ObTransformPyUDFMerge(ObTransformerCtx *ctx);

  virtual ~ObTransformPyUDFMerge();

  virtual int transform_one_stmt(
    ObIArray<ObParentDMLStmt> &parent_stmts,
    ObDMLStmt *&stmt,
    bool &trans_happened) override;

  virtual int merge_python_udf_expr_in_condition(
    ObIArray<int64_t> &idx_list,
    ObIArray<ObRawExpr *> &src_exprs);  

  virtual int extract_python_udf_expr_in_condition(
    ObIArray<ObPythonUdfRawExpr *> &python_udf_expr_list,
    ObIArray<ObRawExpr *> &src_exprs,
    ObIArray<oceanbase::share::schema::ObPythonUDFMeta > &python_udf_meta_list);

  virtual int get_onnx_model_path_from_python_udf_meta(
    ObString &onnx_model_path, 
    oceanbase::share::schema::ObPythonUDFMeta &python_udf_meta);
private:
  virtual int need_transform(
    const common::ObIArray<ObParentDMLStmt> &parent_stmts,
    const int64_t current_level,
    const ObDMLStmt &stmt,
    bool &need_trans) override;

  virtual int construct_transform_hint(ObDMLStmt &stmt, void *trans_params) override;


private:
  ObArenaAllocator allocator_;

};



}
}



#endif // OB_TRANSFORM_PYUDF_MERGE_H