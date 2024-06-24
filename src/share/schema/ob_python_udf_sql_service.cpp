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

#define USING_LOG_PREFIX SHARE_SCHEMA
#include "ob_python_udf_sql_service.h"
#include "lib/oblog/ob_log.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_sql_string.h"
#include "lib/mysqlclient/ob_mysql_proxy.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_python_udf.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"   //?

namespace oceanbase
{
using namespace common;
namespace share
{
namespace schema
{

int ObPythonUdfSqlService::insert_python_udf(const ObPythonUDF &PythonUdf_info,
                                             common::ObISQLClient *sql_client,
                                             const common::ObString *ddl_stmt_str)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(sql_client)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("sql_client is NULL, ", K(ret));
  } else if (!PythonUdf_info.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    SHARE_SCHEMA_LOG(WARN, "PythonUdf_info is invalid", K(PythonUdf_info.get_name_str()), K(ret));
  } else {
    if (OB_FAIL(add_python_udf(*sql_client, PythonUdf_info))) {
      LOG_WARN("failed to add python udf", K(ret));
    } 
    // else {
    //   ObSchemaOperation opt;
    //   opt.tenant_id_ = udf_info.get_tenant_id();
    //   opt.op_type_ = OB_DDL_CREATE_UDF;
    //   opt.schema_version_ = udf_info.get_schema_version();
    //   opt.udf_name_ = udf_info.get_udf_name_str();
    // //   this is a trick. just like outline, synonym
    // //   use table_id_ to store there own id, we use table_id_ to store udf_id and
    // //   table name to store udf name. the reason is table_id_ and table_name_ will
    // //   be write to inner table which named all_ddl_operation, but udf_name_ will not.
    //   opt.table_id_ = udf_info.get_udf_id();
    //   opt.table_name_ = udf_info.get_udf_name_str();
    //   opt.ddl_stmt_str_ = (NULL != ddl_stmt_str) ? *ddl_stmt_str : ObString();
    //   if (OB_FAIL(log_operation(opt, *sql_client))) {
    //     LOG_WARN("Failed to log operation", K(ret));
    //   }
    // }
  }
  return ret;
}

int ObPythonUdfSqlService::add_python_udf(common::ObISQLClient &sql_client,
                                          const ObPythonUDF &PythonUdf_info)
{
  int ret = OB_SUCCESS;
  UNUSED(sql_client);
  UNUSED(PythonUdf_info);
  ObSqlString sql;
  ObSqlString values;
  //const char *tname[] = {OB_ALL_FUNC_TNAME, OB_ALL_FUNC_HISTORY_TNAME};
  std::string table_name = "__all_python_udf";
  ObArenaAllocator allocator(ObModIds::OB_SCHEMA);
  const uint64_t tenant_id = PythonUdf_info.get_tenant_id();
  const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id);
  if (OB_FAIL(sql.assign_fmt("INSERT INTO %s (", table_name.c_str()))) {
      STORAGE_LOG(WARN, "append table name failed, ", K(ret));
  } else {
      LOG_WARN("=========beginning to append sql======== ", K(ret));
      SQL_COL_APPEND_VALUE(sql, values, ObSchemaUtils::get_extract_tenant_id(
                                        exec_tenant_id, PythonUdf_info.get_tenant_id()), "tenant_id", "%lu");
      SQL_COL_APPEND_VALUE(sql, values, ObSchemaUtils::get_extract_schema_id(
                                        exec_tenant_id, PythonUdf_info.get_udf_id()), "udf_id", "%lu");
      SQL_COL_APPEND_ESCAPE_STR_VALUE(sql, values, PythonUdf_info.get_name(),
                                      PythonUdf_info.get_name_str().length(), "name");
      SQL_COL_APPEND_VALUE(sql, values, PythonUdf_info.get_arg_num(), "arg_num", "%d");
      SQL_COL_APPEND_ESCAPE_STR_VALUE(sql, values, PythonUdf_info.get_arg_names(),
                                      PythonUdf_info.get_arg_names_str().length(), "arg_names");
      SQL_COL_APPEND_ESCAPE_STR_VALUE(sql, values, PythonUdf_info.get_arg_types(),
                                      PythonUdf_info.get_arg_types_str().length(), "arg_types");
      SQL_COL_APPEND_VALUE(sql, values, PythonUdf_info.get_ret(), "ret", "%d");
      SQL_COL_APPEND_ESCAPE_STR_VALUE(sql, values, PythonUdf_info.get_pycall(),
                                      PythonUdf_info.get_pycall_str().length(), "pycall");
      SQL_COL_APPEND_VALUE(sql, values, PythonUdf_info.get_schema_version(), "schema_version", "%ld");
      
      if (OB_SUCC(ret)) {
        int64_t affected_rows = 0;
        if (OB_FAIL(sql.append_fmt(") VALUES (%.*s)",
                                   static_cast<int32_t>(values.length()),
                                   values.ptr()))) {
          LOG_WARN("append sql failed, ", K(ret));
        } else if (OB_FAIL(PythonUdf_info.check_pycall())) {
          LOG_WARN("unexpected pycall", K(ret));
        } else if (OB_FAIL(sql_client.write(exec_tenant_id, sql.ptr(), affected_rows))) {
          LOG_WARN("fail to execute sql", K(sql), K(ret));
        } else {
          if (!is_single_row(affected_rows)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unexpected value", K(affected_rows), K(sql), K(ret));
          }
        }
      }
    }
    values.reset();
    return ret;
}

int ObPythonUdfSqlService::delete_python_udf(const uint64_t tenant_id,            
                                             const common::ObString &name,
                                             const int64_t new_schema_version,
                                             common::ObISQLClient *sql_client,
                                             const common::ObString *ddl_stmt_str)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  const int64_t IS_DELETED = 1;
  const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id);

  if (OB_ISNULL(sql_client)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid sql client is NULL", K(ret));
  } else {
    // // insert into __all_udf_history
    // if (FAILEDx(sql.assign_fmt(
    //                "INSERT INTO %s(tenant_id, name, schema_version, is_deleted)"
    //                " VALUES(%lu,'%s',%ld,%ld)",
    //                OB_ALL_FUNC_HISTORY_TNAME,
    //                ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id),
    //                name.ptr(),
    //                new_schema_version, IS_DELETED))) {
    //   LOG_WARN("assign insert into all udf history fail", K(tenant_id), K(ret));
    // } else if (OB_FAIL(sql_client->write(exec_tenant_id, sql.ptr(), affected_rows))) {
    //   LOG_WARN("execute sql fail", K(sql), K(ret));
    // } else if (1 != affected_rows) {
    //   ret = OB_ERR_UNEXPECTED;
    //   LOG_WARN("no row has inserted", K(ret));
    // } else {/*do nothing*/}

    // delete from __all_python_udf
    std::string table_name = "__all_python_udf";
    if (FAILEDx(sql.assign_fmt("DELETE FROM %s WHERE tenant_id = %ld AND name='%s'",
                               table_name.c_str(),
                               ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id),
                               name.ptr()))) {
      LOG_WARN("append_fmt failed", K(ret));
    } else if (OB_FAIL(sql_client->write(exec_tenant_id, sql.ptr(), affected_rows))) {
      LOG_WARN("fail to execute sql", K(tenant_id), K(sql), K(ret));
    } else if (1 != affected_rows) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("no row deleted", K(sql), K(affected_rows), K(ret));
    } else {/*do nothing*/}

    // // log operation
    // if (OB_SUCC(ret)) {
    //   ObSchemaOperation opt;
    //   opt.tenant_id_ = tenant_id;
    //   opt.op_type_ = OB_DDL_DROP_UDF;
    //   opt.schema_version_ = new_schema_version;
    //   opt.udf_name_ = name;
    //   //this is a trick. just like outline, synonym
    //   //use table_id_ to store there own id, we use table name to store
    //   //udf name.
    //   opt.table_name_ = name;
    //   opt.ddl_stmt_str_ = (NULL != ddl_stmt_str) ? *ddl_stmt_str : ObString();
    //   if (OB_FAIL(log_operation(opt, *sql_client))) {
    //     LOG_WARN("Failed to log operation", K(ret));
    //   }
    // }
  }
  return ret;
}


int ObPythonUdfSqlService::drop_python_udf(const ObPythonUDF &udf_info,
                                           const int64_t new_schema_version,
                                           common::ObISQLClient *sql_client,
                                           const common::ObString *ddl_stmt_str)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_ISNULL(sql_client)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid sql client is NULL", K(ret));
  } else if (!udf_info.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid udf info in drop python udf", K(udf_info.get_name_str()), K(ret));
  } else if (OB_FAIL(delete_python_udf(udf_info.get_tenant_id(), udf_info.get_name(),
                                new_schema_version, sql_client, ddl_stmt_str))) {
    LOG_WARN("failed to delete python udf", K(udf_info.get_name_str()), K(ret));
  } else {/*do nothing*/}
  return ret;
}


} //end of schema
} //end of share
} //end of oceanbase
