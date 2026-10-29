#include "identity/identity_manager.h"

#include <utility>

#include "constants.h"
#include "util/logger.h"

namespace sensorswave {

IdentityManager::IdentityManager(IPersistentStore* store, Uuid* uuid, IdentifyEmitter emitter,
                                 SessionRotator sessionRotator)
    : store_(store),
      uuid_(uuid),
      emitter_(std::move(emitter)),
      sessionRotator_(std::move(sessionRotator)) {}

void IdentityManager::load() {
    std::string value;
    if (!store_->read(SW_KEY_ANON_ID, &value) || value.empty()) {
        anonId_ = uuid_->v4();
        persistAnon();  // best-effort persistence: failure only warns, the
                        // in-memory state stands
    } else {
        anonId_ = value;
    }
    if (store_->read(SW_KEY_LOGIN_ID, &value) && !value.empty()) {
        loginId_ = value;  // login state restored across restarts, no event
                           // emitted
    }
}

void IdentityManager::identify(const std::string& loginId) {
    if (loginId.empty()) {
        Logger::warn("identify ignored: empty login id");
        return;
    }
    loginId_ = loginId;
    persistLogin();
    if (emitter_) {
        // $Identify binds anon and login; the facade emitter first syncs the
        // builder identity, then builds the event — identity only fills the
        // top-level fields
        emitter_(anonId_, loginId_);
    }
}

void IdentityManager::setLoginId(const std::string& loginId) {
    if (loginId.empty()) {
        Logger::warn("setLoginId ignored: empty login id");
        return;
    }
    loginId_ = loginId;
    persistLogin();  // persist only, zero events
}

void IdentityManager::reset(bool resetAnonId) {
    loginId_.clear();
    store_->remove(SW_KEY_LOGIN_ID);  // idempotent; failure is expressed by the
                                      // return value, the in-memory logout
                                      // stands regardless
    if (resetAnonId) {
        anonId_ = uuid_->v4();
        persistAnon();
    }
    if (sessionRotator_) {
        sessionRotator_();  // reset opens a new session immediately
    }
}

void IdentityManager::persistAnon() {
    if (!store_->write(SW_KEY_ANON_ID, anonId_)) {
        Logger::warn("persist anon id failed, memory only");
    }
}

void IdentityManager::persistLogin() {
    if (!store_->write(SW_KEY_LOGIN_ID, loginId_)) {
        Logger::warn("persist login id failed, memory only");
    }
}

}  // namespace sensorswave
