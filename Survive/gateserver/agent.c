#include "agent.h"
#include "common/cmdhandler.h"
#include "common/netcmd.h"
#include "gateplayer.h"
#include "chanmsg.h"
#include "togrpgame.h"
#include "kn_thread.h"
#include "config.h"
#include "gateserver.h"
#include "kendynet.h"

#define MAXCMD 65535

static __thread cmd_handler_t handler[MAXCMD] = {NULL};

static __thread agent* t_agent = NULL;


static void release_agent_player(agentplayer_t player){
	printf("release_agent_player\n");
	t_agent->players[player->agentsession.sessionid] = NULL;
	release_id(t_agent->idmgr,player->agentsession.sessionid);
	if(player->actname){
		kn_release_string(player->actname);
	}
	free(player);
}


typedef void (*fn_ref_destroy)(void*);

static agentplayer_t new_agent_player(kn_stream_conn_t conn){
	int id = get_id(t_agent->idmgr);
	if(id <= 0) return NULL;
	else{
		agentplayer_t player = calloc(1,sizeof(*player));
		player->toclient = conn;
		player->state = ply_init;
		kn_ref_init(&player->ref,(fn_ref_destroy)release_agent_player);
		player->agentsession.data = player->ref.identity;
		player->agentsession.aid = t_agent->idx;
		player->agentsession.sessionid = id;
		t_agent->players[id] = player;
		printf("%u,%u\n",player->agentsession.high,player->agentsession.low);
		return player;
	}
}

static agentplayer_t get_agent_player_bysession(agentsession *session){
	if(session){
		agentplayer_t ply = t_agent->players[session->sessionid];
		if(ply && ply->agentsession.data == session->data)
			return ply;
	}
	return NULL;
}

static void forward_game(kn_stream_conn_t con,rpacket_t rpk){
	agentplayer_t ply = (agentplayer_t)kn_stream_conn_getud(con);
	if(ply->gameid){
		wpacket_t wpk = wpk_create_by_rpacket(rpk);
		wpk_write_uint32(wpk,ply->gameid);
		//wpk_write_agentsession(wpk,&ply->agentsession);
		struct chanmsg_forward_game *msg = calloc(1,sizeof(*msg));
		msg->chanmsg.msgtype = FORWARD_GAME;
		msg->game = ply->togame;
		msg->wpk = wpk;
		kn_channel_putmsg(g_togrpgame->chan,NULL,msg,chanmsg_forward_game_destroy);
	}
}


static void send2_game(ident game,wpacket_t wpk){
	struct chanmsg_forward_game *msg = calloc(1,sizeof(*msg));
	msg->chanmsg.msgtype = FORWARD_GAME;
	msg->game = game;
	msg->wpk = wpk;
	kn_channel_putmsg(g_togrpgame->chan,NULL,msg,chanmsg_forward_game_destroy);
}

static void forward_group(kn_stream_conn_t con,rpacket_t rpk){
	agentplayer_t ply = (agentplayer_t)kn_stream_conn_getud(con);
	if(ply->groupid){
		wpacket_t wpk = wpk_create_by_rpacket(rpk);
		wpk_write_uint32(wpk,ply->groupid);
		//wpk_write_agentsession(wpk,&ply->agentsession);
		struct chanmsg_forward_group *msg = calloc(1,sizeof(*msg));
		msg->chanmsg.msgtype = FORWARD_GROUP;
		msg->wpk = wpk;
		kn_channel_putmsg(g_togrpgame->chan,NULL,msg,chanmsg_forward_group_destroy);
		printf("forward_group groupid:%d\n",ply->groupid);
	}
}

static void send2_group(wpacket_t wpk){
	struct chanmsg_forward_group *msg = calloc(1,sizeof(*msg));
	msg->chanmsg.msgtype = FORWARD_GROUP;
	msg->wpk = wpk;
	kn_channel_putmsg(g_togrpgame->chan,NULL,msg,chanmsg_forward_group_destroy);	
}


//处理来自客户端的网络包
static int on_packet(kn_stream_conn_t con,rpacket_t rpk){
	uint16_t cmd = rpk_peek_uint16(rpk);
	printf("process %u\n",cmd);
	if(cmd > CMD_CA_BEGIN && cmd < CMD_CA_END){
		rpk_read_uint16(rpk);
		if(handler[cmd]->_fn) handler[cmd]->_fn(rpk,con);
	}else if(cmd > CMD_CS_BEGIN && cmd < CMD_CS_END){
		 //转发到gameserver
		forward_game(con,rpk);
	}else if(cmd > CMD_CG_BEGIN && cmd < CMD_CG_END){
		//转发到groupserver
		forward_group(con,rpk);
	}
	return 1;
}

static void on_disconnected(kn_stream_conn_t conn,int err){
	agentplayer_t player = kn_stream_conn_getud(conn);
	if(player && player->state != ply_destroy){
		if(player->groupid){
			//通知groupserver player的连接断开
			wpacket_t wpk = NEW_WPK(64);
			wpk_write_uint16(wpk,CMD_AG_CLIENT_DISCONN);
			wpk_write_uint32(wpk,player->groupid);
			send2_group(wpk);
		}
		if(player->gameid){
			//通知gameserver player的连接断开
			wpacket_t wpk = NEW_WPK(64);
			wpk_write_uint16(wpk,CMD_AGAME_CLIENT_DISCONN);
			wpk_write_uint32(wpk,player->gameid);
			send2_game(player->togame,wpk);	
			printf("send CMD_AGAME_CLIENT_DISCONN:%u\n",player->gameid);
		}
		player->groupid = player->gameid = 0;
		make_empty_ident(&player->togame); 
		player->state = ply_destroy;
		kn_ref_release((kn_ref*)player);
	}
}

//处理来自channel的消息
static void on_channel_msg(kn_channel_t chan, kn_channel_t from,void *msg,void *_)
{
	(void)_;
	if(((struct chanmsg*)msg)->msgtype == NEWCLIENT){
		struct chanmsg_newclient *_msg = (struct chanmsg_newclient*)msg;
		agentplayer_t player = new_agent_player(_msg->conn);
		if(player){
			if(0 == kn_stream_server_bind(t_agent->server,_msg->conn,0,4096,on_packet,on_disconnected,
								  0,NULL,0,NULL)){
				kn_stream_conn_setud(_msg->conn,player);
				_msg->conn = NULL;
			}else{
				kn_ref_release((kn_ref*)player);
			}
		}
	}else if(((struct chanmsg*)msg)->msgtype == RPACKET){
		struct chanmsg_rpacket *_msg = (struct chanmsg_rpacket*)msg;
		uint16_t cmd = rpk_peek_uint16(_msg->rpk);
		if((cmd >= CMD_GA_BEGIN && cmd < CMD_GA_END) ||
		   (cmd >= CMD_GAMEA_BEGIN && cmd < CMD_GAMEA_END))
		{
			rpk_read_uint16(_msg->rpk);
			if(handler[cmd]->_fn) handler[cmd]->_fn(_msg->rpk,NULL);
		}else{
			//转发到客户端
			int size = reverse_read_uint32(_msg->rpk);
			int dropsize = size*sizeof(uint64_t)+sizeof(uint32_t);
			//尾部玩家id被创建成一个单独的rpacket
			rpacket_t tmp = rpk_create_skip(_msg->rpk,rpk_len(_msg->rpk)-dropsize);
			//丢弃尾部附加的数据
			rpk_dropback(_msg->rpk,dropsize);
			while(size--){
				agentsession session;
				rpk_read_agentsession(tmp,&session);
				if(session.aid != t_agent->idx) continue;
				agentplayer_t ply = get_agent_player_bysession(&session);
				if(ply){
					if(cmd == CMD_GC_BEGINPLY){
						uint16_t groupid = reverse_read_uint16(_msg->rpk);
						ply->groupid = groupid;
						rpk_dropback(_msg->rpk,sizeof(groupid));
						printf("CMD_GC_GEGINPLY\n");
					}else if(cmd == CMD_SC_ENTERMAP){
						uint16_t groupid = reverse_read_uint16(_msg->rpk);
						rpk_dropback(_msg->rpk,sizeof(groupid));
						uint32_t gameid = reverse_read_uint32(_msg->rpk);
						ply->gameid = gameid;
						ply->groupid = groupid;
						ply->togame = *_msg->game;					
						printf("%x CMD_SC_ENTERMAP:%u\n",ply,gameid);
					}
					if(cmd == CMD_SC_MOV)
						printf("send:CMD_SC_MOV,%u\n",ply->gameid);
					kn_stream_conn_send(ply->toclient,wpk_create_by_rpacket(_msg->rpk));
				}
			}
			rpk_destroy(tmp);
		}
	}
}


int    connect_redis();

static void on_redis_connect(redisconn_t conn,int err,void *_){
	(void)_;
	if(conn){
		t_agent->redis = conn;
		printf("connect to redis success\n");
	}
	else{
		connect_redis();	
	}
}

static	void on_redis_disconnected(redisconn_t conn,void *_){
	(void)_;
	t_agent->redis = NULL;
	connect_redis();	
	printf("on_redis_disconnected\n");
}

int    connect_redis(){
	if(0 != kn_redisAsynConnect(t_agent->p,
				kn_to_cstr(g_config->redisip),g_config->redisport,
				on_redis_connect,
				on_redis_disconnected,
				NULL)){
		//记录日志
		LOG_GATE(LOG_ERROR,"kn_redisAsynConnect return not 0\n");				
		return -1;
	}
	return 0;
}

static void redis_login_cb(redisconn_t _,struct redisReply* reply,void *pridata)
{
	printf("redis cb\n");
	(void)_;
	kn_stream_conn_t conn = (kn_stream_conn_t)pridata;
	agentplayer_t player = (agentplayer_t)kn_stream_conn_getud(conn);
	if(!player){
		kn_stream_conn_close(conn);
		return;
	} 
	if(!reply || reply->type == REDIS_REPLY_ERROR){
		kn_stream_conn_close(conn);
		return;	
	}else{
		
		//const char *chaname = NULL;
		int chaid = 0;
		if(reply->type == REDIS_REPLY_NIL){
			//新用户,无角色
		}else
			chaid = atol(reply->str);
		printf("send CMD_AG_PLYLOGIN\n");	
		player->state = ply_wait_group_confirm;
		wpacket_t wpk = NEW_WPK(128);
		wpk_write_uint16(wpk,CMD_AG_PLYLOGIN);
		wpk_write_string(wpk,kn_to_cstr(player->actname));
		wpk_write_uint32(wpk,chaid);
		wpk_write_agentsession(wpk,&player->agentsession);
		send2_group(wpk);
	}	
}

static void login(rpacket_t rpk,void *ptr){
	printf("login\n");
	kn_stream_conn_t conn = (kn_stream_conn_t)(ptr);
	uint8_t      type = rpk_read_uint8(rpk);//1:设备号,2:帐号
	(void)type;
	const char  *actname = rpk_read_string(rpk);
	agentplayer_t player = kn_stream_conn_getud(conn);
	if(!player){
			kn_stream_conn_close(conn);
			return;
	}
	
	if(player->state != ply_init){
		printf("error state\n"); 
		return;
	}
	
	player->state = ply_wait_verify;
		
	char cmd[1024];
	snprintf(cmd,1024,"get %s",actname);
	if(REDIS_OK!= kn_redisCommand(t_agent->redis,cmd,redis_login_cb,conn)){
		player->state = ply_init;
		printf("kn_redisCommand error\n");
	}else{
		player->actname = kn_new_string(actname);
		printf("send cmd\n");
	}
}


static void group_busy(rpacket_t rpk,void *_){
	(void)_;
	agentsession session;
	rpk_read_agentsession(rpk,&session);
	agentplayer_t ply = get_agent_player_bysession(&session);
	if(ply){
		//通知玩家服务器繁忙
		kn_stream_conn_close(ply->toclient);
	}
}

static void invaild_ply(rpacket_t rpk,void *_){
	(void)_;
	agentsession session;
	rpk_read_agentsession(rpk,&session);
	agentplayer_t ply = get_agent_player_bysession(&session);
	if(ply){
		//通知玩家已经在线
		kn_stream_conn_close(ply->toclient);
	}	
}

static void create_character(rpacket_t rpk,void *_){
	printf("create_character\n");
	(void)_;
	(void)rpk;
	uint32_t groupid = rpk_read_uint32(rpk);
	agentsession session;
	rpk_read_agentsession(rpk,&session);
	printf("%u,%u\n",session.high,session.low);
	agentplayer_t ply = get_agent_player_bysession(&session);
	if(ply){
		printf("CMD_GC_CREATE\n");
		//通知客户端进入创建角色界面
		ply->state = ply_create;
		ply->groupid = groupid;
		wpacket_t wpk = NEW_WPK(64);
		wpk_write_uint16(wpk,CMD_GC_CREATE);
		kn_stream_conn_send(ply->toclient,wpk);
	}	
}

static void reg_handler(){
	REG_C_HANDLER(CMD_CA_LOGIN,login);
	REG_C_HANDLER(CMD_GA_BUSY,group_busy);
	REG_C_HANDLER(CMD_GA_PLY_INVAILD,invaild_ply);
	REG_C_HANDLER(CMD_GA_CREATE,create_character);	
}

static void *service_main(void *ud){
	printf("agent service运行\n");	
	t_agent = (agent*)ud;
	reg_handler();
	if(0 != connect_redis()){
		LOG_GATE(LOG_ERROR,"connect to redis failed,agent thread exit,agentid[%u]\n",t_agent->idx);	
		return NULL;
	}
	while(!t_agent->stop){
		kn_proactor_run(t_agent->p,50);
	}
	return NULL;
}

agent *start_agent(uint8_t idx){
	agent *agent = calloc(1,sizeof(*agent));
	agent->idx = idx;
	agent->p = kn_new_proactor();
	agent->t = kn_create_thread(THREAD_JOINABLE);
	agent->idmgr = new_idmgr(1,4095);
	agent->server=kn_new_stream_server(agent->p,NULL,NULL);
	agent->chan = kn_new_channel();
	kn_channel_bind(agent->p,agent->chan,on_channel_msg,NULL);
	kn_thread_start_run(agent->t,service_main,agent);
	return agent;
}

void   stop_agent(agent *agent){
	agent->stop = 1;
	kn_thread_join(agent->t);
	//stop_agent应该在进程结束时调用，不做任何收尾工作了
}



