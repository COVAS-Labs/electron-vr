#pragma once
#include <cmath>
#include <cstdint>

namespace electron_vr::anchor {
struct Transform {
  float m[3][4]{{1,0,0,0},{0,1,0,0},{0,0,1,0}};
};
inline Transform Multiply(const Transform& a, const Transform& b) {
  Transform out{};
  for (int r=0;r<3;++r) {
    for (int c=0;c<3;++c) {
      out.m[r][c]=0;
      for (int k=0;k<3;++k) out.m[r][c]+=a.m[r][k]*b.m[k][c];
    }
    out.m[r][3]=a.m[r][3];
    for (int k=0;k<3;++k) out.m[r][3]+=a.m[r][k]*b.m[k][3];
  }
  return out;
}
inline Transform Inverse(const Transform& a) {
  Transform out{};
  for (int r=0;r<3;++r) {
    out.m[r][3]=0;
    for (int c=0;c<3;++c) { out.m[r][c]=a.m[c][r]; out.m[r][3]-=a.m[c][r]*a.m[c][3]; }
  }
  return out;
}
inline bool Valid(const Transform& a) {
  for (const auto& row:a.m) for (float v:row) if (!std::isfinite(v)) return false;
  for (int r=0;r<3;++r) for (int s=0;s<3;++s) {
    float dot=0;
    for (int c=0;c<3;++c) dot+=a.m[r][c]*a.m[s][c];
    if (std::fabs(dot-(r==s?1.f:0.f))>0.005f) return false;
  }
  const float determinant = a.m[0][0]*(a.m[1][1]*a.m[2][2]-a.m[1][2]*a.m[2][1]) -
    a.m[0][1]*(a.m[1][0]*a.m[2][2]-a.m[1][2]*a.m[2][0]) +
    a.m[0][2]*(a.m[1][0]*a.m[2][1]-a.m[1][1]*a.m[2][0]);
  return std::fabs(determinant-1.f)<0.005f;
}
inline Transform FromPose(float x,float y,float z,float w,float px,float py,float pz) {
  Transform out;
  out.m[0][0]=1-2*(y*y+z*z); out.m[0][1]=2*(x*y-z*w); out.m[0][2]=2*(x*z+y*w); out.m[0][3]=px;
  out.m[1][0]=2*(x*y+z*w); out.m[1][1]=1-2*(x*x+z*z); out.m[1][2]=2*(y*z-x*w); out.m[1][3]=py;
  out.m[2][0]=2*(x*z-y*w); out.m[2][1]=2*(y*z+x*w); out.m[2][2]=1-2*(x*x+y*y); out.m[2][3]=pz;
  return out;
}
class Follower {
 public:
  void Reset() { source_=0; baseline_={}; prefix_={}; delta_={}; }
  bool Update(uint64_t source,const Transform& current) {
    if (!source || !Valid(current)) return false;
    if (source_!=source) { baseline_=current; prefix_=delta_; source_=source; }
    delta_=Multiply(Multiply(current,Inverse(baseline_)),prefix_);
    return true;
  }
  Transform Place(const Transform& configured) const { return Multiply(delta_,configured); }
 private:
  uint64_t source_=0;
  Transform baseline_,prefix_,delta_;
};
}  // namespace electron_vr::anchor
