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

#ifndef _OB_DROP_MODEL_STMT_H
#define _OB_DROP_MODEL_STMT_H 1

namespace oceanbase
{
namespace sql
{

class ObDropPythonUdfStmt : public ObDDLStmt
{
public:
  ObDropPythonUdfStmt() :
      ObDDLStmt(stmt::T_DROP_PYTHON_UDF),
      drop_python_udf_arg_()
  {}
  ~ObDropPythonUdfStmt() { }
  void set_udf_name(const common::ObString &udf_name) { drop_python_udf_arg_.name_ = udf_name; }
  void set_tenant_id(uint64_t tenant_id) { drop_python_udf_arg_.tenant_id_ = tenant_id; }
  obrpc::ObDropPythonUdfArg &get_drop_python_udf_arg() { return drop_python_udf_arg_; }
  obrpc::ObDDLArg &get_ddl_arg() { return drop_python_udf_arg_; }
  TO_STRING_KV(K_(drop_python_udf_arg));
private:
  obrpc::ObDropPythonUdfArg drop_python_udf_arg_;
  DISALLOW_COPY_AND_ASSIGN(ObDropPythonUdfStmt);

};

}
}

#endif /* _OB_DROP_PYTHON_UDF_STMT_H */