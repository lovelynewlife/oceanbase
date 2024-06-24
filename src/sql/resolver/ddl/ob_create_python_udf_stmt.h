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

#ifndef _OB_CREATE_PYTHON_UDF_STMT_H
#define _OB_CREATE_PYTHON_UDF_STMT_H 1

#include "sql/resolver/ddl/ob_ddl_stmt.h"


namespace oceanbase
{
namespace sql
{

class ObCreatePythonUdfStmt : public ObDDLStmt
{
public:
    ObCreatePythonUdfStmt() :
        ObDDLStmt(stmt::T_CREATE_PYTHON_UDF)
    {}
    ~ObCreatePythonUdfStmt() {}

    obrpc::ObCreatePythonUdfArg &get_create_python_udf_arg() {return create_python_udf_arg_; }

    virtual obrpc::ObDDLArg &get_ddl_arg() {return create_python_udf_arg_; };

private:
    obrpc::ObCreatePythonUdfArg create_python_udf_arg_;  
    DISALLOW_COPY_AND_ASSIGN(ObCreatePythonUdfStmt);  
};

}
}

#endif /* _OB_CREATE_PYTHON_UDF_STMT_H */