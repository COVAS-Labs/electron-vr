#include "../src/openxr_companion_linux.cc"

#include <cstdio>
#include <csignal>

namespace {
bool CheckAck(bool close_after_send, bool valid_sequence, bool valid_generation) {
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) return false;
  vrbridge::g_state.client = sockets[0];
  vrbridge::g_state.snapshot.header.sequence = 17;
  vrbridge::g_state.snapshot.generation = 8;
  electron_vr::openxr_layer_linux::FrameAck ack{};
  using namespace electron_vr::openxr_layer_linux;
  ack.header = {kProtocolMagic, kProtocolVersion, MessageType::kFrameAck, sizeof(ack), 0, valid_sequence ? 17U : 16U};
  ack.generation = valid_generation ? 8U : 7U;
  ack.consumed = 1;
  if (send(sockets[1], &ack, sizeof(ack), 0) != sizeof(ack)) return false;
  if (close_after_send) close(sockets[1]);
  const bool result = vrbridge::ReceiveFrameAckLocked();
  close(sockets[0]);
  if (!close_after_send) close(sockets[1]);
  vrbridge::g_state.client = -1;
  return result;
}
}

int main() {
  const bool normal = CheckAck(false, true, true);
  const bool final_ack = CheckAck(true, true, true);
  const bool stale_sequence_rejected = !CheckAck(true, false, true);
  const bool stale_generation_rejected = !CheckAck(true, true, false);
  std::printf("ACK normal=%u final-before-EOF=%u stale-sequence-rejected=%u stale-generation-rejected=%u\n",
    normal, final_ack, stale_sequence_rejected, stale_generation_rejected);
  return normal && final_ack && stale_sequence_rejected && stale_generation_rejected ? 0 : 1;
}
