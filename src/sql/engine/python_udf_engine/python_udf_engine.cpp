#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/python_udf_engine/python_udf_engine.h"

static const char *fileName = "/home/test/log/batch_buffer_log";

namespace oceanbase
{
    namespace sql 
    {
        //------------------------------------Engine 方法------------------------------------

        //定义静态变量
        pythonUdfEngine* pythonUdfEngine::current_engine = NULL;

        pythonUdfEngine::pythonUdfEngine(/* args */) {
            //Py_InitializeEx(!Py_IsInitialized());
        }

        pythonUdfEngine::~pythonUdfEngine() {
            //用迭代器销毁udf_pool中所有实例
            for(auto iter = udf_pool.begin(); iter != udf_pool.end(); iter ++){
                delete iter->second;
                iter->second = nullptr;
            }
            udf_pool.clear();
            //销毁当前engine
            //Py_FinalizeEx();
            pythonUdfEngine* current_engine = nullptr;
        }

        //填入udf
        bool pythonUdfEngine::insert_python_udf(pid_t tid, pythonUdf *udf) {
            return udf_pool.insert(std::pair<pid_t, pythonUdf*>(tid, udf)).second;
        }
        //从udf_pool中获取已生成udf
        bool pythonUdfEngine::get_python_udf(pid_t tid, pythonUdf *& udf) {
            auto iter = udf_pool.find(tid);
            if(iter != udf_pool.end()){
                udf = iter->second;
                return true;
            } else{
                //从存储中获取udf metadata 并构建udf实例

                //没有该udf名
            }
            return false;
        }

        //------------------------------------UDF 方法------------------------------------
        pythonUdf::pythonUdf(/* args */) {
            pycall = nullptr;
            name = "uninitalized_udf";
            arg_count = 0;
            arg_types = nullptr;
            rt_type = nullptr;
            batch_size = 0; //set in init
            lastTime = 0;
        }

        pythonUdf::~pythonUdf() {
            //释放内存
            name.clear();
            delete pycall;
            pycall = nullptr;
            delete arg_types;
            arg_types = nullptr;
            delete rt_type;
            rt_type = nullptr;

            //释放Python解释器计数
            Py_XDECREF(pModule);
            Py_XDECREF(pFunc);
            Py_XDECREF(dic);
            Py_XDECREF(v);
            Py_XDECREF(pArgs);
            Py_XDECREF(pResult);
            Py_XDECREF(pInitial);
        }

        //初始化udf
        bool pythonUdf::init_python_udf(std::string name, char* pycall, PyUdfSchema::PyUdfArgType* arg_list, int length, PyUdfSchema::PyUdfRetType* rt_type, int batch_size) {
            try{
                this->name = name;
                //利用某种方式获取udf元信息 即代码、参数数量、参数类型和返回值类型
                this->pycall = pycall;
                //设置参数类型和返回值类型
                this->arg_count = length;
                this->arg_types = arg_list;
                this->rt_type = rt_type;
                this->batch_size = batch_size;
                //load main module
                pModule = PyImport_AddModule("__main__");
                if(!pModule)
                    return false;
                //add UDF to the main module
                dic = PyModule_GetDict(pModule);
                if(!dic)
                    return false;
                //test pycall
                v = PyRun_StringFlags(pycall, Py_file_input, dic, dic, NULL);
                if(!v)
                    return false;
                //obtain function pointer called pFunc
                pFunc = PyObject_GetAttrString(pModule, "pyfun");
                if(!pFunc || !PyCallable_Check(pFunc))
                    return false;
                //obtain function pointer initialization
                pInitial = PyObject_GetAttrString(pModule, "pyinitial");
                if(!pInitial || !PyCallable_Check(pInitial))
                    return false;
                //初始化pArgs
                pArgs = PyTuple_New(arg_count);
                if(!pArgs)
                    return false;
                return execute_initial();
            } catch(std::exception &e) {
                return false;
            }
        }
        //获取名字
        std::string pythonUdf::get_name() {
            return name;
        }

        //获取参数数量
        int pythonUdf::get_arg_count() {
            return arg_count;
        }

        //设置参数->重载
        bool pythonUdf::set_arg_at(int i, long const& arg) {
            if((i >= arg_count) | (arg_types[i] != PyUdfSchema::PyUdfArgType::INTEGER)) {
                return false;
            }
            try {
                PyTuple_SetItem(pArgs, i, PyLong_FromLong(arg));
            } catch(std::exception &e) {
                return false;
            }
            return true;
        }
        bool pythonUdf::set_arg_at(int i, bool const& arg) {
            if((i >= arg_count) | (arg_types[i] != PyUdfSchema::PyUdfArgType::BOOLEAN)) {
                return false;
            }
            try {
                if(arg)
                    PyTuple_SetItem(pArgs, i, Py_True);
                else
                    PyTuple_SetItem(pArgs, i, Py_False);
            } catch(std::exception &e) {
                return false;
            }
            return true;
        }
        bool pythonUdf::set_arg_at(int i, double const& arg) {
            if((i >= arg_count) | (arg_types[i] != PyUdfSchema::PyUdfArgType::DOUBLE)) {
                return false;
            }
            try {
                PyTuple_SetItem(pArgs, i, PyFloat_FromDouble(arg));
            } catch(std::exception &e) {
                return false;
            }
            return true;
        }
        bool pythonUdf::set_arg_at(int i, std::string const& arg) {
            if((i >= arg_count) | (arg_types[i] != PyUdfSchema::PyUdfArgType::STRING)) {
                return false;
            }
            try {
                PyTuple_SetItem(pArgs, i, PyUnicode_FromStringAndSize(arg.c_str(), arg.length()));
            } catch(std::exception &e) {
                return false;
            }
            return true;
        }
        bool pythonUdf::set_arg_at(int i, PyObject* arg) {
            try {
                PyTuple_SetItem(pArgs, i, arg);
            } catch(std::exception &e) {
                return false;
            }
            return true;
        }
        //执行初始化代码
        bool pythonUdf::execute_initial() {
            
            //execute pInitial
            try {
                PyObject_CallObject(pInitial, NULL);
            } catch(std::exception &e) {
                return false;
            }
            return true;
        }
        //执行
        bool pythonUdf::execute() {
            //reset
            //pResult = NULL;
            //execute pFun
            try {
                pResult = PyObject_CallObject(pFunc, pArgs);
                if(!pResult) {
                    process_python_exception();
                    return false;
                }
                return true;
            } catch(...) {
                process_python_exception();
                return false;
            }
        }

        //获取执行结果->重载
        bool pythonUdf::get_result(long& result) {
            if((pResult == NULL) | (*rt_type != PyUdfSchema::PyUdfRetType::LONG)){
                return false;
            }
            try {
                result = PyLong_AsLong(pResult);
            } catch(std::exception &e) {
                return false;
            }
            return true;
        }
        bool pythonUdf::get_result(bool& result) {
            if((pResult == NULL) | (*rt_type != PyUdfSchema::PyUdfRetType::BOOLEAN)){
                return false;
            }
            try {
                //

            } catch(std::exception &e) {
                return false;
            }
            return true;
        }
        bool pythonUdf::get_result(double& result) {
            if((pResult == NULL) | (*rt_type != PyUdfSchema::PyUdfRetType::DOUBLE)){
                return false;
            }
            try {
                result = PyFloat_AsDouble(pResult);
            } catch(std::exception &e) {
                return false;
            }
            return true;
        }
        //bytes有不同实现
        bool pythonUdf::get_result(std::string& result) {
            if((pResult == NULL) | (*rt_type != PyUdfSchema::PyUdfRetType::BYTES)){
                return false;
            }
            try {
                result = PyBytes_AsString(pResult);
                //result = PyByteArray_AsString(pResult);
                //result = PyUnicode_AsUTF8(pResult);
            } catch(std::exception &e) {
                return false;
            }
            return true;
        }
        //获取numpy array
        bool pythonUdf::get_result(PyObject*& result) {
            try {
                result = pResult;
            } catch(std::exception &e) {
                return false;
            }
            return true;
        }

        //异常输出
        void pythonUdf::message_error_dialog_show(char* buf) {
            std::ofstream ofile;
            if(ofile)
            {
                ofile.open("/home/test/log/expedia/log", std::ios::out);

                ofile << buf;

                ofile.close();
            }
            return;
        }

        //异常处理
        void pythonUdf::process_python_exception() {
            char buf[65536], *buf_p = buf;
            PyObject *type_obj, *value_obj, *traceback_obj;
            PyErr_Fetch(&type_obj, &value_obj, &traceback_obj);
            if (value_obj == NULL)
                return;

            PyObject *pstr = PyObject_Str(value_obj);

            const char* value = PyUnicode_AsUTF8(pstr);

            size_t szbuf = sizeof(buf);
            int l;
            PyCodeObject *codeobj;

            l = snprintf(buf_p, szbuf, ("Error Message:\n%s"), value);
            buf_p += l;
            szbuf -= l;

            if (traceback_obj != NULL) {
                l = snprintf(buf_p, szbuf, ("\n\nTraceback:\n"));
                buf_p += l;
                szbuf -= l;

                PyTracebackObject *traceback = (PyTracebackObject *)traceback_obj;
                for (; traceback && szbuf > 0; traceback = traceback->tb_next) {
                    //codeobj = traceback->tb_frame->f_code;
                    codeobj = PyFrame_GetCode(traceback->tb_frame);
                    l = snprintf(buf_p, szbuf, "%s: %s(# %d)\n",
                        PyUnicode_AsUTF8(PyObject_Str(codeobj->co_name)),
                        PyUnicode_AsUTF8(PyObject_Str(codeobj->co_filename)),
                        traceback->tb_lineno);
                    buf_p += l;
                    szbuf -= l;
                }
            }

            message_error_dialog_show(buf);

            Py_XDECREF(type_obj);
            Py_XDECREF(value_obj);
            Py_XDECREF(traceback_obj);
            
        }
    }
}