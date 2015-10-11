#include "process.h"
#include "constants.h"
#include "iostream"
#include "fstream"
#include "unistd.h"
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include "limits.h"
#include <assert.h> 
#include "sstream"
#include <algorithm>

using namespace std;

extern pthread_mutex_t fd_lock;
extern pthread_mutex_t up_lock;
extern pthread_mutex_t alive_fd_lock;
extern pthread_mutex_t up_fd_lock;
extern pthread_mutex_t sdr_fd_lock;
extern ReceivedMsgType received_msg_type;
extern pthread_mutex_t log_lock;
extern pthread_mutex_t new_coord_lock;
extern pthread_mutex_t state_req_lock;
extern pthread_mutex_t my_coord_lock;
extern pthread_mutex_t my_state_lock;
extern pthread_mutex_t num_messages_lock;
extern pthread_mutex_t resume_lock;
//sets my_state_ accd to log and calls termination protocol
void Process::Recovery()
{
    LoadLogAndPrevDecisions();

    LoadTransactionId();
    if (transaction_id_ == -1)
        return;

    LoadParticipants();
    LoadUp();
    cout << "Transaction id: " << transaction_id_ << endl;
    cout << "Up set: ";
    for (auto const &p : up_) {
        cout << p << " ";
    }
    cout << endl;
    cout << "Participants: ";
    for (auto const &p : participants_) {
        cout << p << " ";
    }
    cout << endl;


    for (auto const &p : participants_) {
        if (p == get_pid()) continue;
        if (ConnectToProcess(p))
        {
            if(ConnectToProcessSDR(p)){
                ConnectToProcessUp(p);
            }
            else
                cout << "P" << get_pid() << ": Unable to connect sdr to P" << p << endl;

        } else {
            //TODO: I don't think we need to do anything special
            // apart from not adding participant_[i] to the upset.
            cout << "P" << get_pid() << ": Unable to connect to P" << p << endl;
        }
    }

    // pthread_t send_alive_thread;
    // vector<pthread_t> receive_alive_threads(up_.size());
    // CreateAliveThreads(receive_alive_threads, send_alive_thread);

    // one sdr receive thread for each participant, not just those in up_
    // because any participant could ask for Dec Req in future.
    // size = participant_.size()-1 because it contains self
    vector<pthread_t> sdr_receive_threads(participants_.size() - 1);
    vector<pthread_t> up_receive_threads(participants_.size() - 1);
    int i = 0;
    for (auto it = participants_.begin(); it != participants_.end(); ++it) {
        //make sure you don't create a SDR receive thread for self
        if (*it == get_pid()) continue;
        CreateSDRThread(*it, sdr_receive_threads[i]);
        CreateUpThread(*it, up_receive_threads[i]);
        i++;
    }

    string decision = GetDecision();

    //probably need to send the decision to others

    if (decision == "commit")
    {
        my_state_ = COMMITTED;
        cout << "Had received commit" << endl;
    }

    else if (decision == "abort")
    {
        my_state_ = ABORTED;
        cout << "Had received abort" << endl;
    }

    else if (decision == "precommit")
    {
        my_state_ = COMMITTABLE;
        SetUpAndWaitRecovery();
        LogCommitOrAbort();

        // bool local_decreached = false;
        // pthread_mutex_lock(&decision_reached_lock);
        // set_decision_reached(false);
        // pthread_mutex_unlock(&decision_reached_lock);
        // while(!local_decreached)
        // {
        //     sleep(kGeneralSleep);
        //     pthread_mutex_lock(&decision_reached_lock);
        //     local_decreached = get_decision_reached();
        //     pthread_mutex_unlock(&decision_reached_lock);
        // }
        
    }

    else
    {   //no decision
        string vote = GetVote();
        if (vote == "yes")
        {
            cout << "Had voted yes" << endl;
            my_state_ = UNCERTAIN;
            SetUpAndWaitRecovery();
            LogCommitOrAbort();
        }
        else if (vote.empty())
        {
            cout << "Hadnt voted. So aborting" << endl;
            my_state_ = ABORTED;
            LogAbort();
        }
    }

}

void Process::SetUpAndWaitRecovery()
{
    pthread_t decision_request_thread, total_failure_thread;
    CreateThread(decision_request_thread, SendDecReq, (void *)this);
    CreateThread(total_failure_thread, SendUpReq, (void *)this);
    void* status;
    pthread_join(decision_request_thread, &status);
    pthread_join(total_failure_thread, &status);
    RemoveThreadFromSet(decision_request_thread);
    RemoveThreadFromSet(total_failure_thread);
}

void Process::Timeout()
{
    TerminationProtocol();
}

void Process::WaitForDecisionResponse() {
    int n = participants_.size();
    std::vector<pthread_t> receive_thread(n - 1);

    ReceiveDecThreadArgument **rcv_thread_arg = new ReceiveDecThreadArgument*[n - 1];
    int i = 0;
    for (auto it = participants_.begin(); it != participants_.end(); ++it ) {
        if (*it == get_pid()) continue;

        rcv_thread_arg[i] = new ReceiveDecThreadArgument;
        rcv_thread_arg[i]->p = this;
        rcv_thread_arg[i]->pid = *it;
        rcv_thread_arg[i]->transaction_id = transaction_id_;
        // rcv_thread_arg[i]->decision;

        CreateThread(receive_thread[i], ReceiveDecision, (void *)rcv_thread_arg[i]);
        i++;
    }

    void* status;

    i = 0;
    for (auto it = participants_.begin(); it != participants_.end(); ++it ) {
        if (*it == get_pid()) continue;

        pthread_join(receive_thread[i], &status);
        RemoveThreadFromSet(receive_thread[i]);
        if(get_my_state()==COMMITTED || get_my_state()==ABORTED)
            return;
        i++;
    }
}

void* ReceiveDecision(void* _rcv_thread_arg)
{
    ReceiveDecThreadArgument *rcv_thread_arg = (ReceiveDecThreadArgument *)_rcv_thread_arg;
    int pid = rcv_thread_arg->pid;
    int tid = rcv_thread_arg->transaction_id;
    Process *p = rcv_thread_arg->p;

    // ofstream outf("log/decresponse/" + to_string(p->get_pid()) + "from" + to_string(pid));

    // if (!outf.is_open())
    //     cout << "Failed to open log file for decresponse" << endl;

    char buf[kMaxDataSize];
    int num_bytes;
    //TODO: write code to extract multiple messages

    fd_set temp_set;
    FD_ZERO(&temp_set);
    FD_SET(p->get_fd(pid), &temp_set);
    int fd_max = p->get_fd(pid);
    int rv;
    rv = select(fd_max + 1, &temp_set, NULL, NULL, (timeval*)&kTimeout);
    if (rv == -1) { //error in select
        cout << "P" << p->get_pid() << ": ERROR in select() for P" << pid << endl;
        rcv_thread_arg->received_msg_type = ERROR;
    } else if (rv == 0) {   //timeout
        rcv_thread_arg->received_msg_type = TIMEOUT;
    } else {    // activity happened on the socket
        if ((num_bytes = recv(p->get_fd(pid), buf, kMaxDataSize - 1, 0)) == -1) {
            cout << "P" << p->get_pid() << ": ERROR in receiving for P" << pid << endl;
            rcv_thread_arg->received_msg_type = ERROR;
        } else if (num_bytes == 0) {     //connection closed
            cout << "P" << p->get_pid() << ": Connection closed by P" << pid << endl;
            // if participant closes connection, it is equivalent to it crashing
            // can treat it as TIMEOUT
            // TODO: verify argument
            rcv_thread_arg->received_msg_type = TIMEOUT;
            //TODO: handle connection close based on different cases
        } else {
            buf[num_bytes] = '\0';
            cout << "P" << p->get_pid() << ": DecMsg received from P" << pid << ": ";
            if (buf[0] == '0')
                cout << "Abort" << endl;
            else if (buf[1] == '1')
                cout << "Commit" << endl;
            else
                cout << buf << endl;

            string extracted_msg;
            int received_tid;
            // in this case, we don't care about the received_tid,
            // because it will surely be for the transaction under consideration
            p->ExtractMsg(string(buf), extracted_msg, received_tid);

            Decision msg = static_cast<Decision>(atoi(extracted_msg.c_str()));
            // rcv_thread_arg->decision = msg;

            if ((msg) == COMMIT)
            {
                p->set_my_state(COMMITTED);
            }
            else if ((msg) == ABORT)
            {
                p->set_my_state(ABORTED);
            }



            //assumes that correct message type is sent by participant
        }
    }
    // cout << "P" << p->get_pid() << ": Receive thread exiting for P" << pid << endl;
    return NULL;
}

void* SendDecReq(void *_p) {
    Process *p = (Process *)_p;
    p->DecisionRequest();
    return NULL;
}

bool Process::CheckAliveEqualsIntersection()
{
    assert (all_up_sets_.size()!=0);
    set<int> intersection_up(up_);
    set<int> alive_processes_now;
    cout<<"alive_processes_now: ";
    for(auto iter = all_up_sets_.begin(); iter!=all_up_sets_.end(); iter++)
    {
        set_intersection(intersection_up.begin(),intersection_up.end(),iter->second.begin(),iter->second.end(),
                  std::inserter(intersection_up,intersection_up.begin()));
        alive_processes_now.insert(iter->first);
        cout<<iter->first<<" ";
    }
    cout<<endl;
    cout<<"intersection: ";
    for(auto it = intersection_up.begin(); it!=intersection_up.end(); it++)
        cout<<*it<<" ";
    cout<<endl;
    return (intersection_up==alive_processes_now);
}

void Process::DecisionRequest()
{
    string msg;
    string msg_to_send = kDecReq;
    ConstructGeneralMsg(msg_to_send, transaction_id_, msg);
    //if total failure, then init termination protocol with total failure. give arg to TP
    ProcessState local_my_state;

    // sleep(15);
    while (true)
    {
        cout<<"Starting dec req to all "<<endl;
        SendDecReqToAll(msg);
        WaitForDecisionResponse();
        local_my_state = get_my_state();

        if (local_my_state == ABORTED)
        {
            break;
        }
        else if (local_my_state == COMMITTED)
        {
            break;
        }
        // usleep(kDecReqTimeout);
    }
    return;   
}

void Process::TotalFailureCheck()
{
    ProcessState local_my_state;

    while (true)
    {
        // break;
        cout<<"Starting total failure check"<<endl;
        SendUpReqToAll();
        // WaitForUpResponse();
        // cout<<"returned to total failure"<<endl;
        all_up_sets_[get_pid()] = up_; //add mine to allupsets
        usleep(kGeneralTimeout);

        bool is_total_failure = CheckAliveEqualsIntersection();
        if(!is_total_failure)
            cout<<"Intersection doesnt match"<<endl;

        local_my_state = get_my_state();
        
        if (local_my_state == ABORTED)
        {
            break;
        }
        else if (local_my_state == COMMITTED)
        {
            break;
        }
        // usleep(kUpReqTimeout);
    }
}

void Process::TerminationProtocol()
{   //called when a process times out.

    //reset statereq variable
    // state_req_in_progress = true;
    //sets new coord
    cout << "TerminationProtocol by" << get_pid() << " at " << time(NULL) % 100 << endl;
    // bool status = false;
    // while(!status){
    ElectionProtocol();


    // if(my_coordinator_==1)
    //     cout << "P" << pid_ << ": my new coordinator=one"<< endl;
    // else if(my_coordinator_==2)
    //     cout << "P" << pid_ << ": my new coordinator=two"<< endl;

    if (pid_ == get_my_coordinator())
    {   //coord case
        //pass on arg saying total failue, then send to all
        void* status;
        bool templ = false;
        pthread_mutex_lock(&new_coord_lock);

        if (!new_coord_thread_made)
        {
            new_coord_thread_made = true;
            templ = true;
        }

        pthread_mutex_unlock(&new_coord_lock);
        if (templ) {
            CreateThread(newcoord_thread, NewCoordinatorMode, (void *)this);
            pthread_join(newcoord_thread, &status);
            RemoveThreadFromSet(newcoord_thread);
        }

    }
    else
    {
        pthread_mutex_lock(&state_req_lock);
        state_req_in_progress = false;
        pthread_mutex_unlock(&state_req_lock);

        SendURElected(get_my_coordinator());
        usleep(kGeneralTimeout);

        bool temp_sr = false;

        pthread_mutex_lock(&state_req_lock);
        temp_sr = state_req_in_progress;
        pthread_mutex_unlock(&state_req_lock);

        if (temp_sr)
            return;
        TerminationProtocol();

        // WaitForStateRequest();
        //wait for 3 sec
        //check if state req has been received using shared memory


        //then do nothing here, the initial SR thread will see that a SR message is here
        //so that replies state and does all that shit
        //till we get a decision
        // TerminationParticipantMode();
    }
    // }
}

void Process::ElectionProtocol()
{
    int min = GetNewCoordinator();
    set_my_coordinator(min);

}

int Process::GetNewCoordinator()
{
    // ofstream ofile("log/selectnewcoord"+to_string(get_pid())+","+to_string(time(NULL)%100));
    int min;
    pthread_mutex_lock(&up_lock);
    set<int> copy_up(up_);
    pthread_mutex_unlock(&up_lock);

    for ( auto it = copy_up.cbegin(); it != copy_up.cend(); ++it )
    {
        // ofile<<*it;
        if (it == copy_up.cbegin())
            min = (*it);
        else
        {
            if (*it < min)
                min = *it;
        }
    }

    if (min > get_pid())
        min = get_pid();
    // ofile<<"min: "<<min<<endl;
    return min;
}