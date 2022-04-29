#include "connection.h"
#include <mariadb++/connection.hpp>
#include "query.h"
#include "res.h"
#include "mariadb++/exceptions.hpp"
#include "threading.h"
#include "ittnotify.h"
#include <fstream>
#include <sstream>
#include "logger.h"

__itt_domain* domainConnection = __itt_domain_create("connection");


using namespace intercept::client;

class GameDataDBConnection : public game_data {

public:
    GameDataDBConnection() {}
    GameDataDBConnection(const mariadb::account_ref& acct) :
        session(mariadb::connection::create(acct))
    {}
    void lastRefDeleted() const override { delete this; }
    const sqf_script_type& type() const override { return Connection::GameDataDBConnection_type; }
    ~GameDataDBConnection() override {};

    bool get_as_bool() const override { return true; }
    float get_as_number() const override { return 0.f; }
    const r_string& get_as_string() const override { static r_string nm("stuff"sv); return nm; }
    game_data* copy() const override { return new GameDataDBConnection(*this); }
    r_string to_string() const override {
        if (!session) return r_string("<no session>"sv);
        if (!session->connected()) return r_string("<not connected>"sv);
        return "<connected to database: " + session->schema() + ">";
    }
    //virtual bool equals(const game_data*) const override; //#TODO isEqualTo on hashMaps would be quite nice I guess?
    const char* type_as_string() const override { return "databaseConnection"; }
    bool is_nil() const override { return false; }
    bool can_serialize() override { return true; }//Setting this to false causes a fail in scheduled and global vars

    serialization_return serialize(param_archive& ar) override {
        game_data::serialize(ar);
        //size_t entryCount;
        //if (ar._isExporting) entryCount = map.size();
        //ar.serialize("entryCount"sv, entryCount, 1);
        //#TODO add array serialization functions
        //ar._p1->add_array_entry()
        //if (!ar._isExporting) {
        //
        //    for (int i = 0; i < entryCount; ++i) {
        //        s
        //    }
        //
        //
        //
        //}
        return serialization_return::no_error;
    }

    mariadb::connection_ref session;
};

game_data* createGameDataDBConnection(param_archive* ar) {
    auto x = new GameDataDBConnection();
    if (ar)
        x->serialize(*ar);
    return x;
}

static bool is_bool_and(const game_value& v) {
    if (v.is_nil() || v.type_enum() != game_data_type::BOOL) {
        return false;
    }
    return static_cast<bool>(v);
}

bool Connection::throwQueryError(game_state& gs, mariadb::connection_ref connection, size_t errorID, r_string errorMessage, r_string queryString, std::optional<sourcedocpos> sourcePosition) {
    if (connection->account()->hasErrorHandler()) {
        for (auto& it : connection->account()->getErrorHandlers()) {
            auto res = sqf::call(it, { errorMessage, errorID, queryString });
            return is_bool_and(res);
        }
    }
    auto exText = r_string("Intercept-DB exception: ") + errorMessage + "\nat\n" + queryString;
    if (sourcePosition)
        gs.set_script_error(game_state::game_evaluator::evaluator_error_type::foreign,
            exText, *sourcePosition);
    else
        gs.set_script_error(game_state::game_evaluator::evaluator_error_type::foreign,
            exText);
    return false; //error was not handled and we threw it
}

template<class T>
class resolved_promise : public std::promise<T> {
public:
    inline resolved_promise(T value) {
        set_value(value);
    }
};

GameDataDBAsyncResult* Connection::pushAsyncQuery(game_state& gs, mariadb::connection_ref connection, ref<GameDataDBQuery> query) {
    auto gd_res = new GameDataDBAsyncResult();
    gd_res->data = std::make_shared<GameDataDBAsyncResult::dataT>();

    auto_array<game_value> boundValues = query->boundValues;
    r_string queryString = query->getQueryString();
    gd_res->data->statementName = query->isConfigQuery ? query->queryString : r_string{};

    //If we give them to task, it will destruct the array after task is done, and may call dealloc int he pool allocator
    std::vector<
        std::variant<float, bool, r_string, std::monostate>
    > boundValuesQuery;

    uint32_t idx = 0; //Only for error reporting
    for (auto& it : boundValues) {
        if (it.is_null()) {
            boundValuesQuery.emplace_back(std::monostate());
            continue;
        }

        switch (it.type_enum()) {
            case game_data_type::SCALAR: boundValuesQuery.emplace_back(static_cast<float>(it)); break;
            case game_data_type::BOOL: boundValuesQuery.emplace_back(static_cast<bool>(it)); break;
            case game_data_type::STRING: boundValuesQuery.emplace_back(static_cast<r_string>(it)); break;
            case game_data_type::ARRAY: boundValuesQuery.emplace_back(static_cast<r_string>(it)); break;
            default:
                std::stringstream msg;
                msg << "Unsupported bind value type. Got " << intercept::types::__internal::to_string(it.type_enum()) << " on index " << std::to_string(idx);
                throwQueryError(gs, connection, 3, r_string(msg.str()), query->getQueryString());
                //std::promise<bool> prom;
                //auto resFut = prom.get_future();
                resolved_promise<bool> prom(false);
                gd_res->data->fut = prom.get_future();
                return gd_res;
        }
        idx++;
    }

#ifdef NO_SOURCE_POSITION
    std::optional<intercept::types::sourcedocpos> sourcePos(std::nullopt);
#else
    // auto callstackPtr = &gs.get_vm_context()->callstack;
    // auto namePtr = &gs.get_vm_context()->name;
    // auto sourcePtr = &gs.get_vm_context()->sdoc;
    // auto sourcePosPtr = &gs.get_vm_context()->sdocpos;
    // auto source = gs.get_vm_context()->sdoc;
    auto sourcePos = gs.get_vm_context()->sdocpos;
#endif

    if (Logger::get().isThreadLogEnabled()) Logger::get().logThread("pushTask "+queryString);

    gd_res->data->fut = Threading::get().pushTask(connection,
        [queryString, boundValuesQuery, result = gd_res->data, &gs, sourcePos](mariadb::connection_ref con) -> bool {
        try {
            auto statement = con->create_statement(queryString);

            if (statement->get_bind_count() != boundValuesQuery.size()) {
                invoker_lock l;
                throwQueryError(gs, con, 2, 
                    r_string("Invalid number of bind values. Expected "sv)+std::to_string(statement->get_bind_count())+" got "sv+std::to_string(boundValuesQuery.size())
                    , queryString, sourcePos);
                return false;
            }
            uint32_t idx = 0;
            for (auto& it : boundValuesQuery) {
                std::visit([&idx, &statement](auto && arg) {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, float>)
                        statement->set_float(idx++, arg);
                    else if constexpr (std::is_same_v<T, bool>)
                        statement->set_boolean(idx++, arg);
                    else if constexpr (std::is_same_v<T, r_string>)
                        statement->set_string(idx++, arg);
                    else if constexpr (std::is_same_v<T, std::monostate>)
                        statement->set_null(idx++);
                #ifdef _MSVC_LANG
                    else
                        static_assert(false, "non-exhaustive visitor!");
                #endif
                    }, it);
            }

            if (Logger::get().isQueryLogEnabled()) {
                if (boundValuesQuery.empty())
                    Logger::get().logQuery("ASYNC " + queryString);
                else {
                    std::string boundValuesString;
            
                    for (auto& it : boundValuesQuery) {
                        std::visit([&boundValuesString](auto && arg) {
                            using T = std::decay_t<decltype(arg)>;
                            if constexpr (std::is_same_v<T, float> || std::is_same_v<T, bool>)
                                boundValuesString += std::to_string(arg)+ ",";
                            else if constexpr (std::is_same_v<T, r_string>)
                                boundValuesString += arg + ",";
                            else if constexpr (std::is_same_v<T, std::monostate>)
                                boundValuesString += "null,";
                        #ifdef _MSVC_LANG
                            else
                                static_assert(false, "non-exhaustive visitor!");
                        #endif
                        }, it);
                    }
                    boundValuesString.pop_back();
                    boundValuesString += "]";

                    Logger::get().logQuery("ASYNC " + queryString + "\t bound values ["sv +boundValuesString);
                }
            }

            result->res = statement->query();
            return true;
        }
#ifdef NO_SOURCE_POSITION
#define sourcePos std::nullopt
#endif
        catch (mariadb::exception::connection& x) {
            invoker_lock l;
            throwQueryError(gs, con, static_cast<size_t>(x.error_id()), static_cast<r_string>(x.what()), queryString, sourcePos);
            return false;
        }
        catch (std::out_of_range& x) {
            invoker_lock l;
            throwQueryError(gs, con, 1337, static_cast<r_string>(x.what()), queryString, sourcePos);
            return false;
        }
#ifdef NO_SOURCE_POSITION
#undef sourcePos
#endif
    }, true);
    return gd_res;
}

game_value Connection::cmd_createConnectionArray(game_state& gs, game_value_parameter right) {
    if (right.size() < 5) {
         gs.set_script_error(game_state::game_evaluator::evaluator_error_type::dim, 
                r_string("Not enough arguments provided, expected 5 but got "sv)+std::to_string(right.size()));
    }

    r_string ip = right[0];
    int port = right[1];
    r_string user = right[2];
    r_string pw = right[3];
    r_string db = right[4];

    auto acc = mariadb::account::create(ip, user, pw, db, port);

    return new GameDataDBConnection(acc);
}

game_value Connection::cmd_createConnectionConfig(game_state& gs, game_value_parameter right) {
    auto acc = Config::get().getAccount(right);
    if (!acc) {
        sqf::diag_log(r_string("createConnection: script error due to missing account"));
        gs.set_script_error(game_state::game_evaluator::evaluator_error_type::foreign,
            r_string("dbCreateConnection account \"")+static_cast<r_string>(right)+"\" not found in config");
        return {};
    }

    return new GameDataDBConnection(acc);
}


class callstack_item_WaitForQueryResult : public vm_context::callstack_item {

public:

    callstack_item_WaitForQueryResult(ref<GameDataDBAsyncResult> inp, bool scheduled = true) : res(inp), scheduled(scheduled){
    }

    callstack_item_WaitForQueryResult(ref<GameDataDBAsyncResult> inp, ref<vm_context::callstack_item>& parent, size_t stackStart, bool scheduled = true) : res(inp), scheduled(scheduled) {
        setParent(parent);
        setStackDelta(static_cast<int>(stackStart), 1);
    }

    const char* getName() const override { return "idb_wait"; };
    int varCount() const override { return 0; };
    int getVariables(const IDebugVariable** storage, int count) const override { return 0; };
    types::__internal::I_debug_value::RefType EvaluateExpression(const char* code, unsigned rad) override { return {}; };
    void getSourceDocPosition(char* file, int fileSize, int& line) override {
        if (file && fileSize > 0) {
            // strncpy(file, "idb_waitforqueryresult.native", static_cast<size_t>(fileSize));
            file[0] = '\0';
        }
        line = 0;
    };
    IDebugScope* getParent() override { return nullptr; };
    r_string get_type() const override { return "idb_wait"sv; }




    game_instruction* next(int& d1, const game_state* s) override {
        if (res->data->fut.wait_for(std::chrono::nanoseconds(0)) == std::future_status::ready) {
            //push result onto stack.
            auto gd_res = new GameDataDBResult();
            gd_res->res = res->data->res;
            gd_res->statementName = res->data->statementName;
            s->get_vm_context()->scriptStack[_stackEndAtStart] = game_value(gd_res);
            d1 = 2; //done
            //#TODO fix this. Cannot currently because task wants invoker lock, which it won't get while we freeze the game here
        //} else if (!scheduled) {
        //    
        //    res->data->fut.wait();
        //
        //    //push result onto stack.
        //    auto gd_res = new GameDataDBResult();
        //    gd_res->res = res->data->res;
        //    s->get_vm_context()->scriptStack[_stackEndAtStart] = game_value(gd_res);
        //    d1 = 2; //done
        } else {
            d1 = 3; //wait
        }

        return nullptr;
    };
    bool someEH(void* state) override { return false; };
    bool someEH2(void* state) override { return false; };
    void on_before_exec() override {
        
    };

    inline void setParent(ref<vm_context::callstack_item>& newParent) {
        _parent = newParent;
        _varSpace.parent = &newParent->_varSpace;
    }

    inline void setStackDelta(int start, int delta) {
        _stackEndAtStart = start;
        _stackEnd = _stackEndAtStart + delta;
    }

    ref<GameDataDBAsyncResult> res;
    bool scheduled;
};

using namespace intercept::types;

class sourcedoc_fake : public serialize_class {  //See ArmaDebugEngine for more info on this
public:
	sourcedoc_fake():
		sourcefile(""sv),
		content(""sv)
	{
	}
	r_string sourcefile;
	r_string content;

	serialization_return serialize(param_archive& ar) override {
	    return serialization_return::unknown_error;
	}
};

struct vm_context_info {
	auto_array<ref<vm_context::callstack_item>, rv_allocator_local<ref<vm_context::callstack_item>, 64>> callstack;  //#TODO check size on x64
	bool serialenabled;                                                                      //disableSerialization -> true, 0x228
	void* dummyu;                                                                            //VMContextBattlEyeMonitor : VMContextCallback
	
	//const bool is_ui_context; //no touchy
	auto_array<game_value, rv_allocator_local<game_value, 32>> scriptStack;
	
	sourcedoc_fake sdoc;
	// std::pair<r_string, r_string> sdoc;
	
	sourcedocpos sdocpos;  //last instruction pos
	
	r_string name;  //profiler might like this
	
	//breakOut
	r_string breakscopename;
	//throw
	game_value exception_value;  //0x4B0
	//breakOut
	game_value breakvalue;
	uint32_t d[3];
	bool dumm;
	bool dumm2;             //undefined variables allowed?
	const bool scheduled;   //canSuspend 0x4D6
	bool local;
	bool doNil; //undefined variable will be set to nil (unscheduled). If this is false it will throw error
	//throw
	bool exception_state;   //0x4D9
	bool break_;            //0x4DA
	bool breakout;

	inline bool is_scheduled() const {
		return scheduled;
	}
};
template<typename A, typename B>
struct sizes {
	sizes():
		first(sizeof(A)),
		second(sizeof(B))
	{}
	size_t first;
	size_t second;

	operator std::pair<size_t, size_t>() const {
		return { first, second };
	}
};

static inline vm_context_info* context_info(vm_context* ctx) {
	return reinterpret_cast<vm_context_info*>(&ctx->callstack);
}

game_value Connection::cmd_execute(game_state& gs, game_value_parameter con, game_value_parameter qu) {
    auto session = con.get_as<GameDataDBConnection>()->session;
    auto query = qu.get_as<GameDataDBQuery>();

    sizes<sourcedoc, sourcedoc_fake> sourcedoc_sizes;
    sizes<vm_context, vm_context_info> vm_context_sizes;
    auto info = context_info(gs.get_vm_context());
    if (!info->is_scheduled()) { //#TODO just keep using the callstack item but tell it to wait

        try {
            auto statement = session->create_statement(query->getQueryString());

            if (statement->get_bind_count() != query->boundValues.size()) {
                throwQueryError(gs, session, 2,
                    r_string("Invalid number of bind values. Expected "sv) + std::to_string(statement->get_bind_count()) + " got "sv + std::to_string(query->boundValues.size())
                    , query->getQueryString());
                return {};
            }

            uint32_t idx = 0;
            for (auto& it : query->boundValues) {
                if (it.is_null()) {
                    statement->set_null(idx++);
                    continue;
                }

                switch (it.type_enum()) {
                    case game_data_type::SCALAR: statement->set_float(idx++, static_cast<float>(it)); break;
                    case game_data_type::BOOL: statement->set_boolean(idx++, static_cast<bool>(it)); break;
                    case game_data_type::STRING: statement->set_string(idx++, static_cast<r_string>(it)); break;
                    case game_data_type::ARRAY: statement->set_string(idx++, static_cast<r_string>(it)); break;
                    default:
                        throwQueryError(gs, session, 3,
                            r_string("Unsupported bind value type. Got "sv) + intercept::types::__internal::to_string(it.type_enum()) + " on index "sv + std::to_string(idx)
                            + " with value " + static_cast<r_string>(it)
                            ,query->getQueryString());
                        return {};
                }
            }

            if (Logger::get().isQueryLogEnabled()) {
                if (query->boundValues.empty())
                    Logger::get().logQuery(query->getQueryString());
                else {
                    //Yeah this is cheaty, but whatever
                    r_string boundValuesString = static_cast<r_string>(game_value(query->boundValues));

                    Logger::get().logQuery(query->getQueryString() + "\t bound values "sv +boundValuesString);
                }
            }




            auto res = statement->query();

            auto gd_res = new GameDataDBResult();
            gd_res->res = res;
            gd_res->statementName = query->isConfigQuery ? query->queryString : r_string{};
            return gd_res;
        } catch (mariadb::exception::connection& x) {
            throwQueryError(gs, session, static_cast<size_t>(x.error_id()), static_cast<r_string>(x.what()), query->getQueryString());
        } catch (std::out_of_range& x) {
            throwQueryError(gs, session, 1337, static_cast<r_string>(x.what()), query->getQueryString());
        }
        return {}; //burp

    }
    //Set up callstack item to suspend while waiting

    auto& cs = gs.get_vm_context()->callstack;

    auto gd_res = pushAsyncQuery(gs, session, query);

    auto newItem = new callstack_item_WaitForQueryResult(gd_res, cs.back(), gs.get_vm_context()->scriptStack.size() - 2);
    cs.emplace_back(newItem);
    return 123;
}

__itt_string_handle* connection_cmd_executeAsync = __itt_string_handle_create("Connection::cmd_executeAsync");
__itt_string_handle* connection_cmd_executeAsync_task = __itt_string_handle_create("Connection::cmd_executeAsync::task");

game_value Connection::cmd_executeAsync(game_state& gs, game_value_parameter con, game_value_parameter qu) {
    __itt_task_begin(domainConnection, __itt_null, __itt_null, connection_cmd_executeAsync);
    auto session = con.get_as<GameDataDBConnection>()->session;
    auto query = qu.get_as<GameDataDBQuery>();

    auto gd_res = pushAsyncQuery(gs, session, query);
    Threading::get().pushAsyncWork(gd_res);
    __itt_task_end(domainConnection);
    return gd_res;
}

game_value Connection::cmd_ping(game_state& gs, game_value_parameter con) {
    try {
        auto session = con.get_as<GameDataDBConnection>()->session;
        //executes mysql_ping
        auto result = session->connected(); //query("SELECT 1;"sv);

        //return result->next() && result->get_signed64(0) == 1;
        return result;
    } catch (mariadb::exception::connection& x) {
        auto exText = r_string("Intercept-DB ping exception ") + x.what();
        gs.set_script_error(game_state::game_evaluator::evaluator_error_type::foreign,
                exText);
        sqf::diag_log(exText);
        return false;
    }
}

game_value Connection::cmd_isConnected(game_state&, game_value_parameter con) {
    auto session = con.get_as<GameDataDBConnection>()->session;
    return session && (session->connected() || Threading::get().isConnected(session->account()));
}

game_value Connection::cmd_addErrorHandler(game_state&, game_value_parameter con, game_value_parameter handler) {
    auto session = con.get_as<GameDataDBConnection>()->session;
    if (!session) return {};

    session->account()->addErrorHandler(handler);
    return {};
}

game_value Connection::cmd_loadSchema(game_state& gs, game_value_parameter con, game_value_parameter name) {
    

    auto session = con.get_as<GameDataDBConnection>()->session;
    r_string schemaName = name;
    auto schemaPath = Config::get().getSchema(schemaName);

    if (!schemaPath) {
        gs.set_script_error(game_state::game_evaluator::evaluator_error_type::foreign,
            "Schema name not found in config"sv);
        return {};
    }

    if (!std::filesystem::exists(*schemaPath)) {
        gs.set_script_error(game_state::game_evaluator::evaluator_error_type::foreign,
            r_string("Schema file")+schemaPath->string()+" does not exist"sv);
        return {};
    }

    std::ifstream t(*schemaPath);
    std::string str;

    t.seekg(0, std::ios::end);
    str.resize(t.tellg());
    t.seekg(0, std::ios::beg);
    t.read(str.data(), str.length());
    t.close();


    if (!gs.get_vm_context()->is_scheduled()) { 
        try {
            return static_cast<float>(session->execute(static_cast<r_string>(str)));
        }
        catch (mariadb::exception::connection & x) {
            invoker_lock l;
            auto exText = r_string("Intercept-DB exception ") + x.what() + "\nwhile executing loadSchema";
            gs.set_script_error(game_state::game_evaluator::evaluator_error_type::foreign,
                exText);
            sqf::diag_log(exText);
            return {};
        }
    }

    //Set up callstack item to suspend while waiting

    auto& cs = gs.get_vm_context()->callstack;

    auto gd_res = new GameDataDBAsyncResult();
    gd_res->data = std::make_shared<GameDataDBAsyncResult::dataT>();

    gd_res->data->fut = Threading::get().pushTask(session,
        [str, result = gd_res->data, &gs](mariadb::connection_ref con) -> bool {
        try {
            con->execute(static_cast<r_string>(str));
            return true;
        }
        catch (mariadb::exception::connection& x) {
            invoker_lock l;
            //if (con->account()->hasErrorHandler()) {
            //    for (auto& it : con->account()->getErrorHandlers()) {
            //
            //        auto res = sqf::call(it, { static_cast<r_string>(x.what()), static_cast<size_t>(x.error_id()), stmt });
            //
            //        if (res.type_enum() == game_data_type::BOOL && static_cast<bool>(res)) return false; //If returned true then error was handled.
            //    }
            //}
            auto exText = r_string("Intercept-DB exception ") + x.what() + "\nwhile executing loadSchema";
            gs.set_script_error(game_state::game_evaluator::evaluator_error_type::foreign,
                exText);
            sqf::diag_log(exText);

            return false;
        }
    });

    auto newItem = new callstack_item_WaitForQueryResult(gd_res, cs.back(), gs.get_vm_context()->scriptStack.size() - 2);
    cs.emplace_back(newItem);
    return {};
}

void Connection::initCommands() {
    
    auto dbType = host::register_sqf_type("DBCON"sv, "databaseConnection"sv, "TODO"sv, "databaseConnection"sv, createGameDataDBConnection);
    GameDataDBConnection_typeE = dbType.first;
    GameDataDBConnection_type = dbType.second;


    handle_cmd_createConnection = host::register_sqf_command("dbCreateConnection", "TODO", Connection::cmd_createConnectionArray, GameDataDBConnection_typeE, game_data_type::ARRAY);
    handle_cmd_createConnectionConfig = host::register_sqf_command("dbCreateConnection", "TODO", Connection::cmd_createConnectionConfig, GameDataDBConnection_typeE, game_data_type::STRING);
    handle_cmd_execute = host::register_sqf_command("dbExecute", "TODO", Connection::cmd_execute, Result::GameDataDBResult_typeE, GameDataDBConnection_typeE, Query::GameDataDBQuery_typeE);
    handle_cmd_executeAsync = host::register_sqf_command("dbExecuteAsync", "TODO", Connection::cmd_executeAsync, Result::GameDataDBAsyncResult_typeE, GameDataDBConnection_typeE, Query::GameDataDBQuery_typeE);
    handle_cmd_ping = host::register_sqf_command("dbPing", "TODO", Connection::cmd_ping, game_data_type::BOOL, GameDataDBConnection_typeE);
    handle_cmd_isConnected = host::register_sqf_command("dbIsConnected", "TODO", Connection::cmd_isConnected, game_data_type::BOOL, GameDataDBConnection_typeE);
    handle_cmd_addErrorHandler = host::register_sqf_command("dbAddErrorHandler", "TODO", Connection::cmd_addErrorHandler, game_data_type::NOTHING, GameDataDBConnection_typeE, game_data_type::CODE);
    handle_cmd_loadSchema = host::register_sqf_command("dbLoadSchema", "TODO", Connection::cmd_loadSchema, game_data_type::NOTHING, GameDataDBConnection_typeE, game_data_type::STRING);
}
