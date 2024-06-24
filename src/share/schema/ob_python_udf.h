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

#ifndef _OB_PYTHON_UDF_H
#define _OB_PYTHON_UDF_H 1
#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_array.h"
#include "ob_schema_struct.h"
// #include <Python.h>

namespace oceanbase
{
namespace share
{
namespace schema
{

class ObTableSchema;
class ObPythonUDF : public ObSchema
{
    OB_UNIS_VERSION_V(1);

public:              
    //枚举计算结果后返回值类型
    enum PyUdfRetType {
        UDF_UNINITIAL,
        STRING,
        INTEGER,
        REAL,
        DECIMAL
    };

public:
    ObPythonUDF() : ObSchema(), tenant_id_(common::OB_INVALID_ID), udf_id_(common::OB_INVALID_ID), name_(), model_name_(), arg_num_(0), arg_names_(), 
                    arg_types_(), ret_(PyUdfRetType::UDF_UNINITIAL), pycall_(), schema_version_(common::OB_INVALID_VERSION) 
                    { reset(); };
    explicit ObPythonUDF(common::ObIAllocator *allocator);
    ObPythonUDF(const ObPythonUDF &src_schema);
    virtual ~ObPythonUDF();

    //operators
    ObPythonUDF& operator=(const ObPythonUDF &src_schema);
    // bool operator==(const ObPythonUDF &r) const;
    // bool operator!=(const ObPythonUDF &r) const;

    //set methods
    inline void set_tenant_id(const uint64_t id) { tenant_id_ = id; }
    inline void set_udf_id(const uint64_t id) { udf_id_ = id; }
    inline int set_name(const common::ObString &name) { return deep_copy_str(name, name_); }
    inline int set_model_name(const common::ObString &model_name) { return deep_copy_str(model_name, model_name_); }
    inline void set_ret(const enum PyUdfRetType ret) { ret_ = ret; }
    inline void set_ret(const int ret) { ret_ = PyUdfRetType(ret); }
    inline void set_arg_num(const int arg_num) { arg_num_ = arg_num; }
    inline int set_arg_names(const common::ObString arg_names) { return deep_copy_str(arg_names, arg_names_); }
    inline int set_arg_types(const common::ObString arg_types) { return deep_copy_str(arg_types, arg_types_); }
    inline int set_pycall(const common::ObString &pycall) { return deep_copy_str(pycall, pycall_); }
    inline void set_schema_version(int64_t version) { schema_version_ = version; }

    //get methods
    inline uint64_t get_tenant_id() const { return tenant_id_; }
    inline uint64_t get_udf_id() const { return udf_id_; }
    inline const char *get_name() const { return extract_str(name_); }
    inline const common::ObString &get_name_str() const { return name_; }
    inline const char *get_model_name() const { return extract_str(model_name_); }
    inline const common::ObString &get_model_name_str() const { return model_name_; }
    inline int get_arg_num() const { return arg_num_; }
    const char *get_arg_names() const { return extract_str(arg_names_); }
    inline const common::ObString &get_arg_names_str() const { return arg_names_; }
    inline const char *get_arg_types() const { return extract_str(arg_types_); }
    inline const common::ObString &get_arg_types_str() const { return arg_types_; }
    inline enum PyUdfRetType get_ret() const { return ret_; }
    inline const char *get_pycall() const { return extract_str(pycall_); }
    inline const common::ObString &get_pycall_str() const { return pycall_; }
    inline int64_t get_schema_version() const { return schema_version_; }

    //only for retrieve udf
    inline const char *get_udf_name() const { return extract_str(name_); }
    inline common::ObString get_udf_name_str() const { return name_; }
    
    //other
    virtual void reset() override;
    int get_arg_names_arr(common::ObSEArray<common::ObString, 16> &udf_attributes_types) const;
    int get_arg_types_arr(common::ObSEArray<ObPythonUDF::PyUdfRetType, 16> &udf_attributes_types) const;
    int check_pycall() const;

    TO_STRING_KV(K_(tenant_id),
                 K_(udf_id),
                 K_(name),
                 K_(model_name),
                 K_(arg_num),
                 K_(arg_names),
                 K_(arg_types),
                 K_(ret),
                 K_(pycall),
                 K_(schema_version));

public:
    uint64_t tenant_id_;
    uint64_t udf_id_;
    common::ObString name_;
    common::ObString model_name_;
    int arg_num_; //参数数量
    common::ObString arg_names_; //参数名称
    common::ObString arg_types_; //参数类型
    enum PyUdfRetType ret_; //返回值类型
    common::ObString pycall_; //code
    int64_t schema_version_; //the last modify timestamp of this version
};

/////////////////////////////////////////////

class ObPythonUDFMeta
{
  OB_UNIS_VERSION_V(1);
public :
  ObPythonUDFMeta() : name_(), ret_(ObPythonUDF::PyUdfRetType::UDF_UNINITIAL), pycall_(), 
                      udf_attributes_names_(), udf_attributes_types_(), init_(false) {} 
  virtual ~ObPythonUDFMeta() = default;

  void assign(const ObPythonUDFMeta &other) { 
    name_ = other.name_;
    ret_ = other.ret_;
    pycall_ = other.pycall_;
    udf_attributes_names_ = other.udf_attributes_names_;
    udf_attributes_types_ = other.udf_attributes_types_;
    init_ = other.init_;
  }

  ObPythonUDFMeta &operator=(const class ObPythonUDFMeta &other) {
    name_ = other.name_;
    ret_ = other.ret_;
    pycall_ = other.pycall_;
    udf_attributes_names_ = other.udf_attributes_names_;
    udf_attributes_types_ = other.udf_attributes_types_;
    init_ = other.init_;
    return *this;
  }

  TO_STRING_KV(K_(name),
               K_(ret),
               K_(pycall),
               K_(udf_attributes_names),
               K_(udf_attributes_types),
               K_(init));

  common::ObString name_; //函数名
  ObPythonUDF::PyUdfRetType ret_; //返回值类型
  common::ObString pycall_; //code
  common::ObSEArray<common::ObString, 16> udf_attributes_names_; //参数名称
  common::ObSEArray<ObPythonUDF::PyUdfRetType, 16> udf_attributes_types_; //参数类型
  bool init_; //是否已初始化
};

}
}
}

#endif /* _OB_PYTHON_UDF_H */