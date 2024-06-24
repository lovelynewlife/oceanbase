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

#define USING_LOG_PREFIX SQL_RESV
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include "sql/resolver/ddl/ob_create_python_udf_resolver.h"

namespace oceanbase
{
namespace sql
{

ObCreatePythonUdfResolver::ObCreatePythonUdfResolver(ObResolverParams &params)
    : ObDDLResolver(params)
{
}

ObCreatePythonUdfResolver::~ObCreatePythonUdfResolver()
{
}

int ObCreatePythonUdfResolver::resolve_model_path(const ParseNode *parse_node, obrpc::ObCreatePythonUdfArg &create_python_udf_arg)
{
    int ret = OB_SUCCESS;
    std::string model_path = std::string(parse_node->str_value_);
    model_path += "/MLmodel";
    std::ifstream file(model_path);
    std::string inputs = "";
    bool reading_inputs = false;
    //逐行读取
    std::string line;
    while(std::getline(file, line)) {
        if (line.find("inputs") != std::string::npos) {
            reading_inputs = true;
        } 
        if (reading_inputs) {
            if (line.find("outputs") != std::string::npos) {
                break;
            }
            inputs += line;
        }
    }

    //消除inputs中的空格
    inputs.erase(std::remove(inputs.begin(), inputs.end(),' '), inputs.end());

    std::vector<std::string> types;
    std::vector<std::string> names;

    int pos = 0;
    while ((pos = inputs.find("\"type\":\"", pos)) != std::string::npos) {
        pos += sizeof("\"type\":\"") - 1; 
        // Move to the beginning of the value
        size_t end_pos = inputs.find("\"", pos);
        if (end_pos != std::string::npos) {
            types.push_back(inputs.substr(pos, end_pos - pos));
            pos = end_pos;
        }
    }
    pos = 0;
    while ((pos = inputs.find("\"name\":\"", pos)) != std::string::npos) {
        pos += sizeof("\"name\":\"") - 1; 
        // Move to the beginning of the value
        size_t end_pos = inputs.find("\"", pos);
        if (end_pos != std::string::npos) {
            names.push_back(inputs.substr(pos, end_pos - pos));
            pos = end_pos;
        }
    }
    // 将参数类型和参数名字加入到字符串中
    std::string types_str, names_str;
    for (const auto& type : types) {
        types_str += type + ",";
    }
    for (const auto& name : names) {
        names_str += name + ",";
    }
    // 消除最后的逗号
    if (!types_str.empty()) types_str.pop_back();
    if (!names_str.empty()) names_str.pop_back();

    //set types_str
    ret = create_python_udf_arg.python_udf_.set_arg_names(common::ObString(names_str.length(),names_str.c_str()));
    //set arg_types
    ret = create_python_udf_arg.python_udf_.set_arg_types(common::ObString(types_str.length(),types_str.c_str()));
    return ret;
}

int ObCreatePythonUdfResolver::resolve_pycall(const ParseNode *parse_node, obrpc::ObCreatePythonUdfArg &create_python_udf_arg)
{
    int ret = OB_SUCCESS;
    ParseNode *type_node = parse_node->children_[0];
    // 直接以字符串形式引入python_code
    if (T_PYTHON_CODE == type_node->type_) {
        // T_PYTHON_CODE
        // set pycall
        ret = create_python_udf_arg.python_udf_.set_pycall(ObString(type_node->children_[0]->str_len_, type_node->children_[0]->str_value_));
    } 
    else {      //以文件形式引入python_code 
        // T_FILE_PATH
        // get file_path
        const char* file_path = type_node->children_[0]->str_value_;
        // 读取 Python 代码文件
        std::ifstream file(file_path);
        if (!file.is_open()) {
            ret = OB_FILE_NOT_EXIST;
            SQL_RESV_LOG(WARN, "failed to open python code file", K(ret));
        }
        // 将文件内容读入字符串
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string python_code = buffer.str();
        // set pycall
        ret = create_python_udf_arg.python_udf_.set_pycall(common::ObString(python_code.length(),python_code.c_str()));
    }
    return ret;
}

int ObCreatePythonUdfResolver::resolve(const ParseNode &parse_tree)
{
    int ret = OB_SUCCESS;
    ParseNode *create_python_udf_node = const_cast<ParseNode*>(&parse_tree);
    if (OB_ISNULL(create_python_udf_node)
        || T_CREATE_PYTHON_UDF != create_python_udf_node->type_
        || ((4 != create_python_udf_node->num_child_) && (3 != create_python_udf_node->num_child_))       //语法树根节点的孩子数不正确
        || OB_ISNULL(create_python_udf_node->children_)) {
      ret = OB_INVALID_ARGUMENT;
      SQL_RESV_LOG(WARN, "invalid argument.", K(ret));
    } else {
      int children_num = create_python_udf_node->num_child_;
      ObCreatePythonUdfStmt *create_python_udf_stmt = NULL;
      ObString udf_name;
      if (OB_ISNULL(create_python_udf_stmt = create_stmt<ObCreatePythonUdfStmt>())) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        SQL_RESV_LOG(ERROR, "failed to create create_python_udf_stmt", K(ret));
      } else {
        stmt_ = create_python_udf_stmt;
        obrpc::ObCreatePythonUdfArg &create_python_udf_arg = create_python_udf_stmt->get_create_python_udf_arg();
        
        //get udf name
        ParseNode *relation_node = create_python_udf_node->children_[0];
        udf_name = ObString(relation_node->str_len_, relation_node->str_value_);
        //set udf name
        create_python_udf_arg.python_udf_.set_name(udf_name);
        if (4 == children_num) {
            //resolve function element
            ParseNode *function_element_list_node = create_python_udf_node->children_[1];
            //set arg_num
            int arg_num = function_element_list_node->num_child_;
            create_python_udf_arg.python_udf_.set_arg_num(arg_num);
            std::string arg_names = "";
            std::string arg_types = "";
            for (int32_t i = 0; i < arg_num; ++i) {
                //T_PARAM_DEFINITION
                ParseNode *element = function_element_list_node->children_[i];
                //T_IDENT
                ParseNode *arg_name_node = element->children_[0];
                //get arg_name
                const char* arg_name = arg_name_node->str_value_;
                arg_names += arg_name;
                if (i != arg_num - 1) arg_names += ",";
                //get arg type
                ParseNode *type_node = element->children_[1];
                switch (type_node->value_) {
                    case 1:
                        arg_types += "STRING";
                        break;
                    case 2:
                        arg_types += "INTEGER";
                        break;
                    case 3:
                        arg_types += "REAL";
                        break;
                    case 4:
                        arg_types += "DECIMAL";
                        break;       
                }
                if (i != arg_num - 1) arg_types += ",";
            }
            //set arg_names
            create_python_udf_arg.python_udf_.set_arg_names(common::ObString(arg_names.length(),arg_names.c_str()));
            //set arg_types
            create_python_udf_arg.python_udf_.set_arg_types(common::ObString(arg_types.length(),arg_types.c_str()));
            //set return type
            switch (create_python_udf_node->children_[2]->value_) {
                case 1:
                    create_python_udf_arg.python_udf_.set_ret(schema::ObPythonUDF::STRING);
                    break;
                case 2:
                    create_python_udf_arg.python_udf_.set_ret(schema::ObPythonUDF::INTEGER);
                    break;
                case 3:
                    create_python_udf_arg.python_udf_.set_ret(schema::ObPythonUDF::REAL);
                    break;
                case 4:
                    create_python_udf_arg.python_udf_.set_ret(schema::ObPythonUDF::DECIMAL);
                    break;
            }
        } 
        else if (3 == children_num) {
            //resolve model_path
            ParseNode *model_name_node = create_python_udf_node->children_[1];
            ObString model_name = ObString(model_name_node->str_len_, model_name_node->str_value_);
            // ret = resolve_model_path(model_name_node, create_python_udf_arg);
            //set model_name
            create_python_udf_arg.python_udf_.set_model_name(model_name);
        }
        // resolve pycall 
        // T_PYTHON_CODE_TYPE
        ParseNode *pycall_node = create_python_udf_node->children_[children_num - 1];
        ret = resolve_pycall(pycall_node, create_python_udf_arg);
        //set tenant_id
        create_python_udf_arg.python_udf_.set_tenant_id(params_.session_info_->get_effective_tenant_id());
      }
    }            
    return ret;
}

}
}