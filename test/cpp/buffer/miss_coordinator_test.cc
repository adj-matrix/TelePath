#include <cassert>
#include <memory>
#include <thread>

#include "miss_coordinator.h"

namespace {

void AssertFirstRegistrationOwnsAndFollowersJoin() {
  telepath::BufferManagerMissCoordinator coordinator(4);
  const telepath::BufferTag tag{3, 9};

  auto owner = coordinator.RegisterOrJoin(tag);
  auto follower = coordinator.RegisterOrJoin(tag);
  assert(owner.is_owner);
  assert(!follower.is_owner);
  assert(owner.state != nullptr);
  assert(owner.state == follower.state);
}

void AssertCompletionWakesWaiterAndResetsEntry() {
  telepath::BufferManagerMissCoordinator coordinator(4);
  const telepath::BufferTag tag{5, 17};

  auto owner = coordinator.RegisterOrJoin(tag);
  auto follower = coordinator.RegisterOrJoin(tag);
  assert(owner.is_owner);
  assert(!follower.is_owner);

  telepath::Result<telepath::FrameId> wait_result = telepath::Status::Unavailable("not set");
  std::thread waiter([&]() { wait_result = coordinator.Wait(follower.state); });

  coordinator.Complete(tag, owner.state, telepath::Status::Ok(), 42);
  waiter.join();
  assert(wait_result.ok());
  assert(wait_result.value() == 42);

  auto next_owner = coordinator.RegisterOrJoin(tag);
  assert(next_owner.is_owner);
  assert(next_owner.state != owner.state);
}

void AssertWaitRejectsNullAndPropagatesFailure() {
  telepath::BufferManagerMissCoordinator coordinator(4);
  auto null_result = coordinator.Wait(nullptr);
  assert(!null_result.ok());
  assert(null_result.status().code() == telepath::StatusCode::kInvalidArgument);

  const telepath::BufferTag tag{7, 23};
  auto owner = coordinator.RegisterOrJoin(tag);
  auto follower = coordinator.RegisterOrJoin(tag);

  coordinator.Complete(tag, owner.state, telepath::Status::IoError("forced read failure"), telepath::kInvalidFrameId);
  auto wait_result = coordinator.Wait(follower.state);
  assert(!wait_result.ok());
  assert(wait_result.status().code() == telepath::StatusCode::kIoError);
}

}  // namespace

int main() {
  AssertFirstRegistrationOwnsAndFollowersJoin();
  AssertCompletionWakesWaiterAndResetsEntry();
  AssertWaitRejectsNullAndPropagatesFailure();
  return 0;
}
