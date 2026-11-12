#define XR_NO_PROTOTYPES
#include "../seated_anchor.h"
#include "../anchor_transform.h"
#include <iostream>
#include <limits>

namespace {
XrPosef head{{0, 0, 0, 1}, {1, 1.3f, 2}};
XrSpaceLocationFlags flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
int calls = 0;
bool ok = true;
XrResult XRAPI_CALL Locate(XrSpace, XrSpace, XrTime, XrSpaceLocation* location) {
  ++calls;
  location->pose = head;
  location->locationFlags = flags;
  return XR_SUCCESS;
}
void Expect(bool condition, const char* message) {
  if (!condition) { std::cerr << "FAILED: " << message << '\n'; ok = false; }
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }
XrPosef Offset() { return {{0, 0, 0, 1}, {0.5f, -0.2f, -2}}; }
}

int main() {
  using namespace electron_vr::anchor;
  Follower follower;
  const auto configured=FromPose(0,0,0,1,-0.7f,1.26875f,-1.58f);
  auto initial=FromPose(0,std::sin(0.4f),0,std::cos(0.4f),2,1.3f,3);
  Expect(follower.Update(5,initial),"accepts valid application anchor");
  auto world=follower.Place(configured);
  Expect(Near(world.m[0][3],configured.m[0][3]) && Near(world.m[1][3],configured.m[1][3]) &&
    Near(world.m[2][3],configured.m[2][3]),"initial seated mode exactly preserves legacy world placement");
  auto moved=initial; moved.m[0][3]+=0.5f;
  follower.Update(5,moved);
  world=follower.Place(configured);
  Expect(Near(world.m[0][3],-0.2f) && Near(world.m[1][3],1.26875f),"recenter translation moves the configured overlay by the same amount");
  const auto turned=FromPose(0,std::sqrt(0.5f),0,std::sqrt(0.5f),4,1.3f,5);
  follower.Update(5,turned);
  const auto expected=Multiply(Multiply(turned,Inverse(initial)),configured);
  world=follower.Place(configured);
  for(int r=0;r<3;++r)for(int c=0;c<4;++c)Expect(Near(world.m[r][c],expected.m[r][c]),"recenter rotation uses application-relative pose composition");
  for(int i=0;i<10000;++i)follower.Update(5,turned);
  world=follower.Place(configured);
  Expect(Near(world.m[0][3],expected.m[0][3]),"unchanged frames do not accumulate transform drift");
  follower.Update(9,initial);
  world=follower.Place(configured);
  Expect(Near(world.m[0][3],expected.m[0][3]),"changing source preserves the current visible location");
  auto invalid=initial;invalid.m[0][0]=std::numeric_limits<float>::quiet_NaN();
  Expect(!follower.Update(9,invalid),"invalid samples cannot corrupt the anchor");
  follower.Reset();
  world=follower.Place(configured);
  Expect(Near(world.m[0][3],configured.m[0][3]),"mode reset restores the configured world placement");
  const auto view = reinterpret_cast<XrSpace>(1), base = reinterpret_cast<XrSpace>(2);
  const auto app = reinterpret_cast<XrSpace>(3), replacement = reinterpret_cast<XrSpace>(4);
  electron_vr::SeatedAnchor anchor;
  auto pose = Offset();
  Expect(anchor.Apply(view, base, app, 10, Locate, &pose), "captures initial seated pose");
  Expect(Near(pose.position.x, 1.5f) && Near(pose.position.y, 1.1f) && Near(pose.position.z, 0), "offset is relative to captured eye position");
  head.position = {4, 2, 5};
  head.orientation = {0, std::sin(0.5f), 0, std::cos(0.5f)};
  pose = Offset();
  Expect(anchor.Apply(view, base, app, 20, Locate, &pose) && calls == 1, "head motion does not move the anchor or locate every frame");
  Expect(Near(pose.position.x, 1.5f) && Near(pose.position.z, 0), "leaning and turning leave the overlay fixed");
  anchor.ReferenceChanged(40);
  pose = Offset();
  Expect(anchor.Apply(view, base, app, 39, Locate, &pose) && calls == 1, "future recenter is not applied early");
  head.orientation = {0, std::sqrt(0.5f), 0, std::sqrt(0.5f)};
  pose = Offset();
  Expect(anchor.Apply(view, base, app, 40, Locate, &pose) && calls == 2, "recenter captures at changeTime");
  Expect(Near(pose.position.x, 2) && Near(pose.position.y, 1.8f) && Near(pose.position.z, 4.5f), "recenter rotates offsets by yaw and uses new seated position");
  Expect(Near(pose.orientation.y, std::sqrt(0.5f)), "rotation follows captured yaw");
  pose = Offset();
  Expect(anchor.Apply(view, base, replacement, 50, Locate, &pose) && calls == 3, "application reference replacement recaptures generically");
  anchor.Reset();
  flags = 0;
  pose = Offset();
  Expect(!anchor.Apply(view, base, app, 60, Locate, &pose), "missing tracking cannot establish an anchor");
  Expect(Near(pose.position.x, 0.5f), "invalid capture does not modify placement");
  flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  head.position.x = std::numeric_limits<float>::quiet_NaN();
  Expect(!anchor.Apply(view, base, app, 61, Locate, &pose), "rejects non-finite head pose");
  head.position = {0, 1.2f, 0};
  head.orientation = {std::sqrt(0.5f), 0, 0, std::sqrt(0.5f)};
  Expect(!anchor.Apply(view, base, app, 62, Locate, &pose), "cannot establish heading while looking straight up");
  head.orientation = {0, 0, 0, 1};
  Expect(anchor.Apply(view, base, app, 63, Locate, &pose), "tracking recovery retries capture");
  Expect(!anchor.Apply(view, base, XR_NULL_HANDLE, 64, Locate, &pose), "no application projection cannot establish seated rendering");
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  projection.space = app;
  const XrCompositionLayerBaseHeader* layers[] = {nullptr, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
  XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};
  frame.layerCount = 2; frame.layers = layers;
  Expect(electron_vr::ProjectionSpace(frame) == app, "selects application projection and tolerates null layers");
  frame.layers = nullptr;
  Expect(electron_vr::ProjectionSpace(frame) == XR_NULL_HANDLE, "invalid layer array is not dereferenced");
  if (ok) std::cout << "Seated anchor tests passed\n";
  return ok ? 0 : 1;
}
