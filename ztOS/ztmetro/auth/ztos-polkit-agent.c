#define _GNU_SOURCE
#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include <polkitagent/polkitagent.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

typedef struct { PolkitAgentListener parent; } ZtAgent;
typedef struct { PolkitAgentListenerClass parent; } ZtAgentClass;
G_DEFINE_TYPE(ZtAgent, zt_agent, POLKIT_AGENT_TYPE_LISTENER)

typedef struct {
    GTask *task;
    PolkitIdentity *identity;
    PolkitAgentSession *session;
    GCancellable *cancel;
    gulong cancel_handler;
    char *cookie, *message;
    int fd, attempts;
    guint watch;
    char input[1024]; size_t used;
    gboolean prompted, done, gained;
} Request;

static void start_session(Request *r);
static void finish(Request *r, gboolean gained);
static void send_line(Request *r, const char *kind, const char *message) {
    char line[512];
    int n=snprintf(line,sizeof(line),"%s\t%.400s\n",kind,message);
    send(r->fd,line,n,MSG_NOSIGNAL);
}
static gboolean finish_done(gpointer data) {
    Request *r=data;
    if(r->cancel_handler) g_signal_handler_disconnect(r->cancel,r->cancel_handler);
    if(r->watch) g_source_remove(r->watch);
    if(r->fd>=0) close(r->fd);
    explicit_bzero(r->input,sizeof(r->input));
    g_task_return_boolean(r->task,TRUE);
    g_clear_object(&r->task); g_clear_object(&r->identity);
    g_clear_object(&r->session); g_clear_object(&r->cancel);
    g_free(r->cookie); g_free(r->message); g_free(r);
    return G_SOURCE_REMOVE;
}
static void finish(Request *r,gboolean gained) {
    if(r->done) return;
    r->done=TRUE; r->gained=gained;
    if(r->watch) { g_source_remove(r->watch); r->watch=0; }
    if(r->fd>=0) send_line(r,gained?"OK":"FAIL",gained?"Password accepted":"Authentication cancelled or failed");
    g_timeout_add(r->fd>=0?650:1,finish_done,r);
}
static void cancelled(GCancellable *cancel,gpointer data) {
    Request *r=data;
    if(r->done) return;
    if(r->session) polkit_agent_session_cancel(r->session);
    finish(r,FALSE);
}
static gboolean answer(int fd,GIOCondition condition,gpointer data) {
    Request *r=data;
    if(condition&(G_IO_HUP|G_IO_ERR)) { r->watch=0; cancelled(NULL,r); return G_SOURCE_REMOVE; }
    ssize_t n=read(fd,r->input+r->used,sizeof(r->input)-1-r->used);
    if(n<0 && (errno==EINTR||errno==EAGAIN)) return G_SOURCE_CONTINUE;
    if(n<=0) { r->watch=0; cancelled(NULL,r); return G_SOURCE_REMOVE; }
    r->used+=n; r->input[r->used]=0;
    char *end=strchr(r->input,'\n');
    if(!end && r->used<sizeof(r->input)-1) return G_SOURCE_CONTINUE;
    if(!end || strncmp(r->input,"PASSWORD\t",9)) { r->watch=0; cancelled(NULL,r); return G_SOURCE_REMOVE; }
    *end=0;
    polkit_agent_session_response(r->session,r->input+9);
    explicit_bzero(r->input,sizeof(r->input)); r->used=0;
    return G_SOURCE_CONTINUE;
}
static void password_request(PolkitAgentSession *session,const char *prompt,gboolean echo,gpointer data) {
    Request *r=data;
    if(r->done) return;
    send_line(r,r->prompted?"FAIL":"PROMPT",r->prompted?"Incorrect password. Try again.":r->message);
    r->prompted=TRUE;
}
static gboolean after_session(gpointer data) {
    Request *r=data;
    if(r->done) return G_SOURCE_REMOVE;
    if(!r->gained && r->attempts<3) start_session(r);
    else finish(r,r->gained);
    return G_SOURCE_REMOVE;
}
static void completed(PolkitAgentSession *session,gboolean gained,gpointer data) {
    Request *r=data;
    if(!r->done) { r->gained=gained; g_idle_add(after_session,r); }
}
static void start_session(Request *r) {
    if(r->session) { g_signal_handlers_disconnect_by_data(r->session,r); g_clear_object(&r->session); }
    r->attempts++;
    r->session=polkit_agent_session_new(r->identity,r->cookie);
    g_signal_connect(r->session,"request",G_CALLBACK(password_request),r);
    g_signal_connect(r->session,"completed",G_CALLBACK(completed),r);
    polkit_agent_session_initiate(r->session);
}
static void initiate(PolkitAgentListener *listener,const char *action,const char *message,
        const char *icon,PolkitDetails *details,const char *cookie,GList *identities,
        GCancellable *cancel,GAsyncReadyCallback callback,gpointer user_data) {
    Request *r=g_new0(Request,1); r->fd=-1;
    r->task=g_task_new(listener,cancel,callback,user_data);
    if(!identities) { finish(r,FALSE); return; }
    PolkitIdentity *identity=identities->data;
    for(GList *p=identities;p;p=p->next)
        if(POLKIT_IS_UNIX_USER(p->data)&&polkit_unix_user_get_uid(p->data)==getuid()) identity=p->data;
    r->identity=g_object_ref(identity); r->cookie=g_strdup(cookie);
    char *name=polkit_identity_to_string(identity);
    r->message=g_strdup_printf("%.180s — %.60s",message,name); g_free(name);
    g_strdelimit(r->message,"\t\r\n",' ');
    if(cancel) { r->cancel=g_object_ref(cancel); r->cancel_handler=g_signal_connect(cancel,"cancelled",G_CALLBACK(cancelled),r); }
    const char *runtime=g_getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    if(!runtime||snprintf(addr.sun_path,sizeof(addr.sun_path),"%s/ztos-auth.sock",runtime)>=sizeof(addr.sun_path)) { finish(r,FALSE); return; }
    r->fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(r->fd<0||connect(r->fd,(struct sockaddr*)&addr,sizeof(addr))) { finish(r,FALSE); return; }
    r->watch=g_unix_fd_add(r->fd,G_IO_IN|G_IO_HUP|G_IO_ERR,answer,r);
    start_session(r);
}
static gboolean initiate_finish(PolkitAgentListener *listener,GAsyncResult *result,GError **error) {
    return g_task_propagate_boolean(G_TASK(result),error);
}
static void zt_agent_class_init(ZtAgentClass *klass) {
    PolkitAgentListenerClass *listener=POLKIT_AGENT_LISTENER_CLASS(klass);
    listener->initiate_authentication=initiate;
    listener->initiate_authentication_finish=initiate_finish;
}
static void zt_agent_init(ZtAgent *self) {}
int main(void) {
    prctl(PR_SET_PDEATHSIG,SIGTERM);
    GError *error=NULL;
    PolkitSubject *subject=polkit_unix_session_new_for_process_sync(getpid(),NULL,&error);
    if(!subject) { g_printerr("ztos-polkit-agent: %s\n",error->message); return 1; }
    PolkitAgentListener *agent=g_object_new(zt_agent_get_type(),NULL);
    gpointer registration=polkit_agent_listener_register(agent,POLKIT_AGENT_REGISTER_FLAGS_NONE,
        subject,"/org/ztos/AuthenticationAgent",NULL,&error);
    if(!registration) { g_printerr("ztos-polkit-agent: %s\n",error->message); return 1; }
    GMainLoop *loop=g_main_loop_new(NULL,FALSE); g_main_loop_run(loop);
    polkit_agent_listener_unregister(registration);
    return 0;
}
