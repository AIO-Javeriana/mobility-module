#include "./libs/src/sio_client.h"
#include "./libs/json.hpp"
#include "CommunicationEvents.cpp"

#include <functional>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <queue>
#include <map>

#ifdef WIN32

#define HIGHLIGHT(__O__) std::cout<<__O__<<std::endl
#define EM(__O__) std::cout<<__O__<<std::endl
#include <stdio.h>
#include <tchar.h>

#else

#define HIGHLIGHT(__O__) std::cout<<"\e[1;31m"<<__O__<<"\e[0m"<<std::endl
#define EM(__O__) std::cout<<"\e[1;30;1m"<<__O__<<"\e[0m"<<std::endl

#endif

using json = nlohmann::json;
using namespace sio;
using namespace std;



class connection_listener
{
    sio::client &handler;
    std::mutex _lock;
    std::condition_variable_any _cond;
    bool connect_finished;

public:

    connection_listener(sio::client& h):
    handler(h)
    {
      connect_finished=false;
    }
    void on_connected()
    {
        _lock.lock();
        _cond.notify_all();
        connect_finished = true;
        _lock.unlock();
    }
    void on_close(client::close_reason const& reason)
    {
        std::cout<<"sio closed "<<std::endl;
        exit(0);
    }

    void on_fail()
    {
        std::cout<<"sio failed "<<std::endl;
        exit(0);
    }

    condition_variable_any* getCond(){
        return &_cond;
    }

    bool getConnectFinished(){
      return connect_finished;
    }

    mutex* getLock(){
      return &_lock;
    }

};


class CommunicationChannel{
    sio::client h;
    connection_listener l;
    socket::ptr current_socket;
    Services services;
    bool subscribed;
    string module_id;
    string url="";
public:


    CommunicationChannel(string host,int port,Services services):l(h){
       this->subscribed=false;
        this->url=host+":"+to_string(port);
        this->services=services;
    }

    void start(){
        h.set_open_listener(std::bind(&connection_listener::on_connected, &l));
        h.set_close_listener(std::bind(&connection_listener::on_close, &l,std::placeholders::_1));
        h.set_fail_listener(std::bind(&connection_listener::on_fail, &l));
        h.connect(url);
        l.getLock()->lock();
        if(!l.getConnectFinished())
        {
            l.getCond()->wait(*l.getLock());
        }
        l.getLock()->unlock();
        current_socket = h.socket();
        bind_events();
    }

   void subscription(string &module_info){
            current_socket->emit(toString(CommunicationEvents::REGISTRATION), module_info);
    }

    void bind_events(){
		   current_socket->on(toString(CommunicationEvents::REGISTRATION_REPLY), sio::socket::event_listener_aux([&](string const& name, message::ptr const& data, bool isAck,message::list &ack_resp){
                l.getLock()->lock();
        				json dataJSON=json::parse((data->get_string()));
        				this->module_id=dataJSON["MODULE_ID"];
                        HIGHLIGHT("MODULE SUBSCRIBED \n ID "<<this->module_id);//;
        				// EM(user<<":"<<message);
        				this->subscribed=true;

        				l.getCond()->notify_all();
                l.getLock()->unlock();
                current_socket->off("login");
            }));
			current_socket->on(toString(CommunicationEvents::WORK_ASSIGNATION), sio::socket::event_listener_aux([&](string const& name, message::ptr const& data, bool isAck,message::list &ack_resp){
                l.getLock()->lock();
        				json dataJSON=json::parse((data->get_string()));
                EM("\t WORK-ASSING "<<dataJSON);
        				json reply_info={
        					  {"MODULE_ID",this->module_id},
        					  {"REPLY","ACCEPTED"},
                    {"COMMAND_ID",dataJSON["COMMAND_ID"]},
                    {"GROUP_ID",dataJSON["GROUP_ID"]}
                };
                if (!this->services.assessWork(dataJSON)){
                  reply_info["REPLY"]="REFUSE";
                }
        				current_socket->emit(toString(CommunicationEvents::WORK_ASSIGNATION_REPLY), reply_info.dump() );
    		        l.getLock()->unlock();
            }));
      current_socket->on(toString(CommunicationEvents::ALL_BEGINS), sio::socket::event_listener_aux([&](string const& name, message::ptr const& data, bool isAck,message::list &ack_resp){
                l.getLock()->lock();
                json dataJSON=json::parse((data->get_string()));
                EM("\t ALL-BEGINS "<<dataJSON);
                json reply_info={
                    {"MODULE_ID",this->module_id},
                    {"STATUS","DONE"},//error
                    {"MSG",""}
                    //{"COMMAND_ID",dataJSON["COMMAND_ID"]}

                };
                string reply="";
                this->services.setWorking(true);
                while (!services.isEmptyCommands(dataJSON["GROUP_ID"])){
                      //_lock.lock();
                      CommandInfo command=services.makeWorkFront(dataJSON["GROUP_ID"]);
                      reply_info["COMMAND_ID"]=command.getCommand()["COMMAND_ID"];
                      reply_info["GROUP_ID"]=command.getCommand()["GROUP_ID"];
                      if(!command.isError()){
                        reply_info["STATUS"]="DONE";
                      }else{
                          reply_info["STATUS"]="ERROR";
                      }
                      current_socket->emit(toString(CommunicationEvents::ACTION_FINISHED), reply_info.dump() );
                      //_lock.unlock();
                }
                std::cout << "ACTION-FINISH" << std::endl;
                this->services.setWorking(false);
                l.getLock()->unlock();
            }));
            current_socket->on(toString(CommunicationEvents::WORK_STATUS), sio::socket::event_listener_aux([&](string const& name, message::ptr const& data, bool isAck,message::list &ack_resp){
                      l.getLock()->lock();
              				json dataJSON=json::parse((data->get_string()));
                      EM("\t WORK-STATUS "<<dataJSON);
              				json reply_info={
              					  {"MODULE_ID",this->module_id},
              					  {"STATUS","ERROR"},
                          {"COMMAND_ID",dataJSON["COMMAND_ID"]},
                          {"GROUP_ID",dataJSON["GROUP_ID"]},
                          {"MSG",""}
                      };
                      if (!this->services.isWorking()){
                        reply_info["STATUS"]="WORKING";
                      }
              				current_socket->emit(toString(CommunicationEvents::WORK_STATUS_REPLY), reply_info.dump() );
          		        l.getLock()->unlock();
                  }));


        }

    void setURL(string url){
        this->url=url;
    }

    connection_listener* getConnection_listenert(){
        return &l;
    }

    socket::ptr getCurrent_socket(){
        return current_socket;
    }

    void endConnection(){
      h.sync_close();
      h.clear_con_listeners();
	  current_socket->off_all();
      current_socket->off_error();
    }

    std::mutex* getLock(){
      return this->l.getLock();
    }

    ~CommunicationChannel(){
      cout<<"Destructor CommunicationChannel"<<endl;
      current_socket->off_all();
      current_socket->off_error();
      h.sync_close();
      h.clear_con_listeners();
      //delete h;
    }

    };
