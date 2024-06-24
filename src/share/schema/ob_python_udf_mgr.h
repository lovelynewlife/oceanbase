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

#ifndef OB_PYTHON_UDF_MGR_H_
#define OB_PYTHON_UDF_MGR_H_

#include "lib/hash/ob_hashmap.h"
#include "lib/hash/ob_pointer_hashmap.h"
#include "lib/container/ob_vector.h"
#include "lib/allocator/page_arena.h"
#include "share/schema/ob_python_udf.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

class ObSimplePythonUdfSchema : public ObSchema
{
public:
  ObSimplePythonUdfSchema();
  explicit ObSimplePythonUdfSchema(common::ObIAllocator *allocator);
  ObSimplePythonUdfSchema(const ObSimplePythonUdfSchema &src_schema);
  virtual ~ObSimplePythonUdfSchema();
  ObSimplePythonUdfSchema &operator =(const ObSimplePythonUdfSchema &other);
  TO_STRING_KV(K_(tenant_id),
               K_(udf_id),
               K_(udf_name),
               K_(arg_num),
               K_(arg_names),
               K_(arg_types),
               K_(ret),
               K_(pycall),
               K_(schema_version));
  virtual void reset();
  inline bool is_valid() const;
  inline int64_t get_convert_size() const;
  inline void set_tenant_id(const uint64_t tenant_id) { tenant_id_ = tenant_id; }
  inline uint64_t get_tenant_id() const { return tenant_id_; }
  inline void set_udf_id(const uint64_t udf_id) { udf_id_ = udf_id; }
  inline uint64_t get_udf_id() const { return udf_id_; }
  inline void set_schema_version(const int64_t schema_version) { schema_version_ = schema_version; }
  inline int64_t get_schema_version() const { return schema_version_; }
  inline int set_udf_name(const common::ObString &name)
  { return deep_copy_str(name, udf_name_); }
  inline const char *get_udf_name() const { return extract_str(udf_name_); }
  inline const common::ObString &get_udf_name_str() const { return udf_name_; }
  inline common::ObString get_udf_name_str() { return udf_name_; }
  inline ObTenantPythonUdfId get_tenant_python_udf_id() const
  { return ObTenantPythonUdfId(tenant_id_, udf_name_); }

  //the ObSimpleUDFSchema must have the same interface just like ObUDF.
  //ObSchemaRetrieveUtils's template function will use these interface.
  inline int set_name(const common::ObString &name) { return deep_copy_str(name, udf_name_); }
  inline void set_ret(const enum ObPythonUDF::PyUdfRetType ret) { ret_ = ret; }
  inline void set_ret(const int ret) { ret_ = ObPythonUDF::PyUdfRetType(ret); }
  inline void set_arg_num(const int arg_num) { arg_num_ = arg_num; }
  inline int set_arg_names(const common::ObString arg_names) { return deep_copy_str(arg_names, arg_names_); }
  inline int set_arg_types(const common::ObString arg_types) { return deep_copy_str(arg_types, arg_types_); }
  inline int set_pycall(const common::ObString &pycall) { return deep_copy_str(pycall, pycall_); }

  inline const char *get_name() const { return extract_str(udf_name_); }
  inline const common::ObString &get_name_str() const { return udf_name_; }
  inline int get_arg_num() const { return arg_num_; }
  const char *get_arg_names() const { return extract_str(arg_names_); }
  inline const common::ObString &get_arg_names_str() const { return arg_names_; }
  inline const char *get_arg_types() const { return extract_str(arg_types_); }
  inline const common::ObString &get_arg_types_str() const { return arg_types_; }
  inline enum ObPythonUDF::PyUdfRetType get_ret() const { return ret_; }
  inline const char *get_pycall() const { return extract_str(pycall_); }
  inline const common::ObString &get_pycall_str() const { return pycall_; }

private:
  uint64_t tenant_id_;
  uint64_t udf_id_;
  common::ObString udf_name_;
  int arg_num_;
  common::ObString arg_names_;
  common::ObString arg_types_;
  enum ObPythonUDF::PyUdfRetType ret_;
  common::ObString pycall_;
  int64_t schema_version_;
};

template<class T, class V>
struct ObGetPythonUDFKey {
  void operator()(const T &t, const V &v) const {
    UNUSED(t);
    UNUSED(v);
  }
};

class ObPythonUDFHashWrapper
{
public:
  ObPythonUDFHashWrapper()
    : tenant_id_(common::OB_INVALID_ID),
      udf_name_() {}
  ObPythonUDFHashWrapper(uint64_t tenant_id, const common::ObString &udf_name)
    : tenant_id_(tenant_id),
      udf_name_(udf_name) {}
  ~ObPythonUDFHashWrapper() {}
  inline uint64_t hash() const
  {
    uint64_t hash_ret = 0;
    hash_ret = common::murmurhash(&tenant_id_, sizeof(uint64_t), 0);
    hash_ret = common::murmurhash(get_udf_name().ptr(), get_udf_name().length(), hash_ret);
    return hash_ret;
  }
  inline bool operator==(const ObPythonUDFHashWrapper &rv) const{
    return (tenant_id_ == rv.get_tenant_id())
        && (udf_name_ == rv.get_udf_name());
  }
  inline void set_tenant_id(uint64_t tenant_id) { tenant_id_ = tenant_id; }
  inline void set_udf_name(const common::ObString &udf_name) { udf_name_ = udf_name; }
  inline uint64_t get_tenant_id() const { return tenant_id_; }
  inline const common::ObString &get_udf_name() const { return udf_name_; }
  TO_STRING_KV(K_(tenant_id), K_(udf_name));

private:
  uint64_t tenant_id_;
  common::ObString udf_name_;
};

template<>
struct ObGetPythonUDFKey<ObPythonUDFHashWrapper, ObSimplePythonUdfSchema *>
{
  ObPythonUDFHashWrapper operator() (const ObSimplePythonUdfSchema * udf) const {
    ObPythonUDFHashWrapper hash_wrap;
    if (!OB_ISNULL(udf)) {
      hash_wrap.set_tenant_id(udf->get_tenant_id());
      hash_wrap.set_udf_name(udf->get_udf_name());
    }
    return hash_wrap;
  }
};


class ObPythonUDFMgr
{
public:
  typedef common::ObSortedVector<ObSimplePythonUdfSchema *> UDFInfos;
  typedef common::hash::ObPointerHashMap<ObPythonUDFHashWrapper, ObSimplePythonUdfSchema *, ObGetPythonUDFKey> ObUDFMap;
  typedef UDFInfos::iterator UDFIter;
  typedef UDFInfos::const_iterator ConstUDFIter;
  ObPythonUDFMgr();
  explicit ObPythonUDFMgr(common::ObIAllocator &allocator);
  virtual ~ObPythonUDFMgr();
  int init();
  void reset();
  ObPythonUDFMgr &operator =(const ObPythonUDFMgr &other);
  int assign(const ObPythonUDFMgr &other);
  int deep_copy(const ObPythonUDFMgr &other);
  void dump() const;
  int get_python_udf_schema_count(int64_t &udf_schema_count) const;
  int get_schema_statistics(ObSchemaStatisticsInfo &schema_info) const;
  int add_python_udf(const ObSimplePythonUdfSchema &udf_schema);
  int add_python_udfs(const common::ObIArray<ObSimplePythonUdfSchema> &udf_schema);
  int del_python_udf(const ObTenantPythonUdfId &udf);

  int get_python_udf_schema(const uint64_t udf_id,
                            const ObSimplePythonUdfSchema *&udf_schema) const;
  int get_python_udf_info_version(uint64_t udf_id, int64_t &udf_version) const;
  int get_python_udf_schema_with_name(const uint64_t tenant_id,
                                      const common::ObString &name,
                                      const ObSimplePythonUdfSchema *&udf_schema) const;
  int get_python_udf_schemas_in_tenant(const uint64_t tenant_id,
                                       common::ObIArray<const ObSimplePythonUdfSchema *> &udf_schemas) const;
  int del_schemas_in_tenant(const uint64_t tenant_id);
  inline static bool compare_python_udf(const ObSimplePythonUdfSchema *lhs,
                                 const ObSimplePythonUdfSchema *rhs) {
    return lhs->get_tenant_python_udf_id() < rhs->get_tenant_python_udf_id();
  }
  inline static bool equal_python_udf(const ObSimplePythonUdfSchema *lhs,
                                      const ObSimplePythonUdfSchema *rhs) {
    return lhs->get_tenant_python_udf_id() == rhs->get_tenant_python_udf_id();
  }
  static int rebuild_python_udf_hashmap(const UDFInfos &udf_infos,
                                        ObUDFMap &udf_map);
private:
  inline static bool compare_with_tenant_python_udf_id(const ObSimplePythonUdfSchema *lhs,
                                                       const ObTenantPythonUdfId &tenant_outline_id);
  inline static bool equal_to_tenant_python_udf_id(const ObSimplePythonUdfSchema *lhs,
                                                   const ObTenantPythonUdfId &tenant_outline_id);
private:
  bool is_inited_;
  common::ObArenaAllocator local_allocator_;
  common::ObIAllocator &allocator_;
  UDFInfos udf_infos_;
  ObUDFMap udf_map_;
};


} //end of schema
} //end of share
} //end of oceanbase

#endif