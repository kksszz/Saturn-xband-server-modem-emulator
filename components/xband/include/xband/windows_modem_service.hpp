#pragma once
#include <xband/windows_tcp_client.hpp>
#include <xband/staged_service_batch.hpp>

namespace xband::windows {
// One emulator-owned service connection. All methods are owner-thread only.
// step performs bounded nonblocking work: no wait loop, sleep or guest execution.
// Pump even while the guest is paused. Guest scheduling and AT replies belong
// to the owner; connected means open_ok was received, not just TCP connected.
class ModemServiceClient final {
public:
    enum class State {connecting,idle,opening,connected,transferring,closing,stopped};
    enum class Failure {none,transport,control,batch,exception,close_busy,close_rejected,drain};
    explicit ModemServiceClient(ClientConfig config,uint64_t now)
        :client_(std::move(config),now) {}
    ModemServiceClient(const ModemServiceClient&)=delete;
    ModemServiceClient& operator=(const ModemServiceClient&)=delete;
    ~ModemServiceClient(){stop();}
    State state()const noexcept{return state_;}
    TcpClient::Failure transportFailure()const noexcept{return client_.failure();}
    Failure failure()const noexcept{return failure_;}
    State failureState()const noexcept{return failureState_;}
    const char *failureName()const noexcept{
        switch(failure_){
        case Failure::none:return "stopped without a recorded fault";
        case Failure::transport:return TcpClient::failureName(client_.failure());
        case Failure::control:return "control state stopped";
        case Failure::batch:return "service batch failed";
        case Failure::exception:return "service exception";
        case Failure::close_busy:return "close attempted during pending work";
        case Failure::close_rejected:return "close request rejected";
        case Failure::drain:return "response drain failed";
        }
        return "unknown service failure";
    }
    size_t pending()const noexcept{return batch_?batch_->pending():0;}
    bool carrier()const noexcept{return !closeRequested_&&(state_==State::connected||state_==State::transferring);}
    bool readyToRun()const noexcept{return !closeRequested_&&state_==State::connected&&batch_&&batch_->readyToRun();}
    bool closePending()const noexcept{return closeRequested_||state_==State::closing;}
    bool appendWaitSockets(fd_set &readable,fd_set &writable)const noexcept{
        // begin() only stages bytes. No socket event can start this local work.
        if(localWork_)return false;
        return client_.appendWaitSockets(readable,writable);
    }
    bool step(uint64_t now)noexcept{
        if(state_==State::stopped)return false;
        localWork_=false;
        // TcpClient failure destroys its control. Do not cancel a batch that
        // borrowed that control afterwards; discard it without dereferencing.
        if(!client_.step(now))return fail(Failure::transport);
        try{
            auto *control=client_.control();
            if(!control)return true;
            using C=protocol::ClientControl::State;
            if(control->state()==C::stopped)return fail(Failure::control);
            if(state_==State::connecting&&control->state()==C::idle)state_=State::idle;
            else if(state_==State::opening&&control->state()==C::service){
                batch_=std::make_unique<StagedServiceBatch<>>(*control);
                state_=State::connected;
            }else if(state_==State::closing&&control->state()==C::idle){
                batch_.reset();state_=State::idle;
            }else if(state_==State::transferring){
                const auto result=batch_->resume(now);
                localWork_=result==RemoteServiceBatch::Step::progress;
                if(result==RemoteServiceBatch::Step::failed)return fail(Failure::batch);
                if(result==RemoteServiceBatch::Step::done)state_=State::connected;
            }
            if(closeRequested_){
                if(state_==State::idle)closeRequested_=false;
                else if(state_==State::connected&&!close(now))return false;
            }
            return true;
        }catch(...){return fail(Failure::exception);}
    }
    bool open(std::string_view subscriber,uint64_t now){
        if(closeRequested_||state_!=State::idle)return false;
        if(client_.control()->open(std::string(subscriber),now)!=protocol::Admission::queued)return false;
        state_=State::opening;return true;
    }
    // Copies input before returning; caller can then clear its TX queue.
    // A full UART does not block network completion. Retain response backlog
    // in staging and drain only at a guest-time boundary approved by the owner.
    bool transfer(std::span<const uint8_t> bytes,uint64_t guestTick){
        if(closeRequested_||state_!=State::connected||!batch_->begin(bytes,guestTick))return false;
        state_=State::transferring;localWork_=true;return true;
    }
    template<class Sink> size_t drain(Sink sink,size_t budget=256)noexcept{
        if(!readyToRun())return 0;
        const auto count=batch_->drain(sink,budget);
        if(batch_->failed())fail(Failure::drain);
        return count;
    }
    // Pending open/transfer cannot be retracted. Cancel the entire connection
    // instead of allowing a stale CONNECT or replaying partially sent bytes.
    bool close(uint64_t now){
        if(state_==State::idle)return true;
        if(state_!=State::connected)return fail(Failure::close_busy);
        if(client_.control()->close(now)!=protocol::Admission::queued)return fail(Failure::close_rejected);
        batch_.reset();state_=State::closing;return true;
    }
    // ATZ/ATH may arrive while an open or transfer is already on the wire.
    // Finish that operation once, discard its old response, then queue close.
    // No new open/transfer or guest response is allowed until close_ok. This
    // is not cancellation/rollback, replay, automatic reconnect or recovery:
    // transport errors and the existing request deadlines remain fatal.
    bool requestClose(uint64_t now){
        if(state_==State::stopped)return false;
        if(state_==State::idle)return true;
        closeRequested_=true;
        return state_!=State::connected||close(now);
    }
    // Reset/disable/shutdown: no handshake wait, automatic reconnect or retry.
    // Destroy borrowed-control users before the transport owner.
    void stop()noexcept{batch_.reset();client_.stop();state_=State::stopped;localWork_=closeRequested_=false;}
private:
    bool fail(Failure why)noexcept{
        failure_=why;failureState_=state_;stop();return false;
    }
    TcpClient client_;
    std::unique_ptr<StagedServiceBatch<>> batch_;
    State state_=State::connecting;
    bool localWork_=false;
    bool closeRequested_=false;
    Failure failure_=Failure::none;
    State failureState_=State::connecting;
};
}
