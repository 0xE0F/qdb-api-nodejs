
#pragma once

#include <node.h>
#include <node_object_wrap.h>

#include "cluster_data.hpp"
#include "blob.hpp"
#include "integer.hpp"
#include "queue.hpp"
#include "hset.hpp"
#include "tag.hpp"

namespace qdb
{

    // cAmelCaSe :(
    class Cluster : public node::ObjectWrap 
    {

    public:
        // we have a structure we can ref count that holds important data
        // this makes sure we can keep things alive in asynchronous operations

    public:
        explicit Cluster(const char * uri) : _uri(new std::string(uri)) {}
        virtual ~Cluster(void) {}

    public:
        static void Init(v8::Handle<v8::Object> exports)
        {
            v8::Isolate* isolate = v8::Isolate::GetCurrent();

            // Prepare constructor template
            v8::Local<v8::FunctionTemplate> tpl = v8::FunctionTemplate::New(isolate, New);
            tpl->SetClassName(v8::String::NewFromUtf8(isolate, "Cluster"));
            tpl->InstanceTemplate()->SetInternalFieldCount(1);

            // Prototype
            NODE_SET_PROTOTYPE_METHOD(tpl, "connect", connect);
            NODE_SET_PROTOTYPE_METHOD(tpl, "blob", blob);
            NODE_SET_PROTOTYPE_METHOD(tpl, "integer", integer);
            NODE_SET_PROTOTYPE_METHOD(tpl, "queue", queue);
            NODE_SET_PROTOTYPE_METHOD(tpl, "set", set);
            NODE_SET_PROTOTYPE_METHOD(tpl, "tag", tag);

            constructor.Reset(isolate, tpl->GetFunction());
            exports->Set(v8::String::NewFromUtf8(isolate, "Cluster"), tpl->GetFunction());
        }

    private:
        static void New(const v8::FunctionCallbackInfo<v8::Value>& args)
        {
            if (args.IsConstructCall()) 
            {
                MethodMan call(args);

                if (args.Length() != 1) 
                {
                    call.throwException("Expected exactly one argument");
                    return;
                }

                auto str = call.checkedArgString(0);
                if (!str.second)
                {
                    call.throwException("Expected a connection string as first argument");
                    return;
                }

                v8::String::Utf8Value utf8str(str.first);

                // the cluster only owns the uri
                // when we will connect we will create a reference counted cluster_data
                // with a handle
                // because the cluster_data is reference counted and transmitted to every 
                // callback we are sure it is kept alive for as long as needed
                Cluster * cl = new Cluster(*utf8str);

                cl->Wrap(args.This());
                args.GetReturnValue().Set(args.This());
            } 
            else 
            {
                v8::Isolate* isolate = v8::Isolate::GetCurrent();
                // Invoked as plain function `MyObject(...)`, turn into construct call.
                const int argc = 1;
                v8::Local<v8::Value> argv[argc] = { args[0] };
                v8::Local<v8::Function> cons = v8::Local<v8::Function>::New(isolate, constructor);
                args.GetReturnValue().Set(cons->NewInstance(argc, argv));
            }
        }

    public:
        template <typename Object>
        static void newObject(const v8::FunctionCallbackInfo<v8::Value> & args)
        {
            if (args.IsConstructCall()) 
            {
                MethodMan call(args);

                if (args.Length() != 2) 
                {
                    call.throwException("Wrong number of arguments");
                    return;
                }

                // the first parameter is a Cluster object, let's unwrap it to get access to the underlying handle
                auto obj = call.checkedArgObject(0);

                if (!obj.second)
                {
                    call.throwException("Invalid parameter supplied to object");
                    return;
                } 

                auto str = call.checkedArgString(1);
                if (!str.second)
                {
                    call.throwException("Expected a string as an argument");
                    return;
                }

                v8::String::Utf8Value utf8str(str.first);

                // get access to the underlying handle
                Cluster * c = ObjectWrap::Unwrap<Cluster>(obj.first);

                cluster_data_ptr data = c->data();
                if (!data)
                {
                    call.throwException("Cluster is not connected");
                    return;
                }

                Object * b = new Object(data, *utf8str);

                b->Wrap(args.This());
                args.GetReturnValue().Set(args.This());
            } 
            else 
            {
                v8::Isolate* isolate = v8::Isolate::GetCurrent();
                // Invoked as plain function `MyObject(...)`, turn into construct call.
                const int argc = 2;
                v8::Local<v8::Value> argv[argc] = { args[0], args[1] };
                v8::Local<v8::Function> cons = v8::Local<v8::Function>::New(isolate, constructor);
                args.GetReturnValue().Set(cons->NewInstance(argc, argv));
            }
        }

    private:
        template <typename Object>
        static void objectFactory(const v8::FunctionCallbackInfo<v8::Value> & args)
        {
            v8::Isolate* isolate = v8::Isolate::GetCurrent();
            v8::HandleScope scope(isolate);

            v8::Local<v8::Object> the_cluster = args.Holder();

            const int argc = 2;
            v8::Local<v8::Value> argv[argc] = { the_cluster, args[0] };
            v8::Local<v8::Function> cons = v8::Local<v8::Function>::New(isolate, Object::constructor);
            args.GetReturnValue().Set(cons->NewInstance(argc, argv));        
        }

    private:
        struct connection_request
        {
            explicit connection_request(cluster_data_ptr cd) : data(cd) 
            {
                assert(data);
            }

            cluster_data_ptr data;
            qdb_error_t error;

            void execute(void)
            {
                error = data->connect();
            }            
        };

    private:
        static void processConnectionResult(uv_work_t * req, int status) 
        {
            v8::Isolate * isolate = v8::Isolate::GetCurrent();

            v8::TryCatch try_catch;

            connection_request * conn_req = static_cast<connection_request *>(req->data);
            assert(conn_req);

            cluster_data_ptr cd = conn_req->data;     
            assert(cd);           

            const qdb_error_t err = (status < 0) ? qdb_e_internal : conn_req->error;

            // no longer needs the request object
            delete conn_req;

            // if we got any kind of error call "on error", otherwise call "on success"
            if (err == qdb_e_ok)
            {
                cd->on_success(isolate);
            }
            else
            {
                cd->on_error(isolate, err);
            }            

            if (try_catch.HasCaught())
            {
                node::FatalException(try_catch);
            }                       
        }

    private:
        static void callback_wrapper(uv_work_t * req)
        {
            static_cast<connection_request *>(req->data)->execute();
        }

    public:
        static void connect(const v8::FunctionCallbackInfo<v8::Value>& args)
        {
            MethodMan call(args);

            if (args.Length() != 2) 
            {
                call.throwException("Wrong number of arguments");
                return;
            }

            auto on_success = call.checkedArgCallback(0);

            if (!on_success.second)
            {
                call.throwException("Expected a callback as first argument");
                return;
            }

            auto on_error = call.checkedArgCallback(1);

            if (!on_error.second)
            {
                call.throwException("Expected a callback as second argument");
                return;                    
            }

            Cluster * c = call.nativeHolder<Cluster>();
            assert(c);

            uv_work_t * work = new uv_work_t();
            work->data = new connection_request(c->new_data(on_success.first, on_error.first));
            
             uv_queue_work(uv_default_loop(),
                work,
                &Cluster::callback_wrapper,
                &Cluster::processConnectionResult);    
        }

    public:
        static void blob(const v8::FunctionCallbackInfo<v8::Value> & args) 
        {
            objectFactory<Blob>(args);
        }

        static void integer(const v8::FunctionCallbackInfo<v8::Value> & args) 
        {
            objectFactory<Integer>(args);
        }
        
        static void queue(const v8::FunctionCallbackInfo<v8::Value> & args) 
        {
            objectFactory<Queue>(args);
        }

        static void set(const v8::FunctionCallbackInfo<v8::Value> & args) 
        {
            objectFactory<Set>(args);
        }

        static void tag(const v8::FunctionCallbackInfo<v8::Value> & args) 
        {
            objectFactory<Tag>(args);
        }

    public:
        const std::string & uri(void) const
        {
            assert(_uri);
            return *_uri;
        }

    public:
        cluster_data_ptr data(void)
        {
            return _data;
        }

    private:
        cluster_data_ptr new_data(v8::Local<v8::Function> on_success, v8::Local<v8::Function> on_error)
        {
            assert(_uri);
            return _data = std::make_shared<cluster_data>(*_uri, on_success, on_error);
        }

    private:
        std::unique_ptr<std::string> _uri;
        cluster_data_ptr _data;

        static v8::Persistent<v8::Function> constructor;
    };

}

