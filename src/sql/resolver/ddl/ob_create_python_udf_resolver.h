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

#ifndef _OB_CREATE_MODEL_RESOLVER_H
#define _OB_CREATE_MODEL_RESOLVER_H 1

#include "sql/resolver/ddl/ob_create_python_udf_stmt.h"
#include "sql/resolver/ddl/ob_ddl_resolver.h"
#include "share/ob_rpc_struct.h"
#include "share/schema/ob_schema_struct.h"

namespace oceanbase
{
namespace sql
{

class ObCreatePythonUdfResolver : public ObDDLResolver
{
public:
  explicit ObCreatePythonUdfResolver(ObResolverParams &params);
  virtual ~ObCreatePythonUdfResolver();

  virtual int resolve(const ParseNode &parse_tree);
  int resolve_model_path(const ParseNode *parse_node, obrpc::ObCreatePythonUdfArg &create_python_udf_arg);
  int resolve_pycall(const ParseNode *parse_node, obrpc::ObCreatePythonUdfArg &create_python_udf_arg);
};

}
}

#endif /* _OB_CREATE_MODEL_RESOLVER_H */