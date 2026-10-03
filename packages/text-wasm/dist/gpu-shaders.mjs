// Ports of VideoCut's MetalTextSdf, MetalGaussian and MetalQtTextSoftGlow.
// Source hashes and experimental scope are recorded in reports/gpu-port.md.
export const fullscreen = /* wgsl */ `
struct Quad { @builtin(position) position: vec4f, @location(0) uv: vec2f };
@vertex fn quad(@builtin(vertex_index) i: u32) -> Quad {
  let p = array<vec2f,3>(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));
  var o: Quad; o.position = vec4f(p[i],0,1); o.uv = vec2f((p[i].x+1)*0.5,(1-p[i].y)*0.5); return o;
}
`;
export const sdf = /* wgsl */ `
struct DistanceInput { @location(0) position: vec2f, @location(1) parabola: vec2f,
  @location(2) limits: vec2f, @location(3) scale: f32, @location(4) limit: f32 };
struct DistanceVaryings { @builtin(position) position: vec4f, @location(0) parabola: vec2f,
  @location(1) limits: vec2f, @location(2) normalizedScale: f32 };
@vertex fn distanceVertex(v: DistanceInput) -> DistanceVaryings {
  var o: DistanceVaryings; o.position=vec4f(2*v.position.x-1,1-2*v.position.y,0.5,1);
  o.parabola=v.parabola; o.limits=v.limits; o.normalizedScale=v.scale/v.limit; return o;
}
struct DistanceOutput { @location(0) color: vec4f, @builtin(frag_depth) depth: f32 };
fn pack16(value: f32) -> vec2f { let e=fract(vec2f(256,1)*value); return e-e.xx*vec2f(0,0.00390625); }
@fragment fn distanceFragment(v: DistanceVaryings) -> DistanceOutput {
  let p=0.5-v.parabola.y; let q=-0.5*v.parabola.x;
  let signX=select(-1.0,1.0,v.parabola.x>0); let squaredQ=(27*q)*q; let cubedP=((4*p)*p)*p;
  let thirdP=(-p)*0.3333333432674408; var distance: f32;
  if (squaredQ >= -cubedP) {
    let middle=signX*pow(sqrt(abs(squaredQ+cubedP))*0.09622500091791153+0.5*abs(q),0.3333333432674408);
    // The analytic cusp can be exactly zero. Its limiting root is zero.
    var root=0.0; if (abs(middle)>1e-20) { root=thirdP/middle+middle; }
    root=clamp(root,v.limits.x,v.limits.y); distance=length(vec2f(root,root*root)-v.parabola);
  } else {
    let ratioSquared=abs(squaredQ/cubedP); let ratio=sqrt(ratioSquared);
    let approximation=ratioSquared*(0.018753239884972572*ratio-0.08179157972335815)+(0.3309875428676605*ratio+1.7320507764816284);
    var root0=(signX*sqrt(abs(thirdP)))*approximation;
    let deltaRoot=signX*sqrt(max(0.0,((-0.75*root0)*root0)-p));
    var root1=(-0.5*root0)-deltaRoot; root0=clamp(root0,v.limits.x,v.limits.y); root1=clamp(root1,v.limits.x,v.limits.y);
    distance=min(length(vec2f(root0,root0*root0)-v.parabola),length(vec2f(root1,root1*root1)-v.parabola));
  }
  let d=min(distance*v.normalizedScale,1.0); var o: DistanceOutput;
  o.color=vec4f(pack16(0.5-0.5*d),0,1); o.depth=d; return o;
}
struct ShapeVaryings { @builtin(position) position: vec4f, @location(0) parabola: vec2f };
@vertex fn shapeVertex(@location(0) position: vec2f,@location(1) parabola: vec2f) -> ShapeVaryings {
  var o: ShapeVaryings; o.position=vec4f(2*position.x-1,1-2*position.y,0.5,1); o.parabola=parabola; return o;
}
@fragment fn shapeFragment(v: ShapeVaryings) -> @location(0) vec4f {
  if (!(v.parabola.x*v.parabola.x<v.parabola.y)) { discard; } return vec4f(1);
}
@fragment fn inverseFragment() -> @location(0) vec4f { return vec4f(1); }
`;
export const effects =
  fullscreen +
  /* wgsl */ `
struct Params { a: vec4f, b: vec4f, c: vec4f, d: vec4f };
@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var linearSampler: sampler;
@group(0) @binding(2) var<uniform> u: Params;
@group(0) @binding(3) var auxiliary: texture_2d<f32>;
fn sampled(uv: vec2f) -> vec4f {
  var c=textureSampleLevel(source,linearSampler,uv,0);
  if (u.b.x>0.5) { c=vec4f(pow(c.rgb,vec3f(u.b.y)),c.a); } return c;
}
fn gaussianAt(uv: vec2f) -> vec4f {
  if (u.a.x<0.00001) { return textureSampleLevel(source,linearSampler,uv,0); }
  let center=sampled(uv); var sum=center; var normalization=1.0;
  for (var i=1; i<=1024; i++) {
    if (f32(i)>u.a.x) { break; }
    let delta=f32(i)*u.a.z; let weight=exp(((-0.5*delta)*delta)/(u.a.y*u.a.y));
    let offset=select(vec2f(delta,0),vec2f(0,delta),u.a.w>0.5);
    let minus=uv-offset; let plus=uv+offset;
    let low=select(minus.x,minus.y,u.a.w>0.5); let high=select(plus.x,plus.y,u.a.w>0.5);
    if (low>=0) { sum+=sampled(minus)*weight; normalization+=weight; }
    if (high<=1) { sum+=sampled(plus)*weight; normalization+=weight; }
  }
  sum/=normalization;
  if (u.b.x>0.5) { sum=vec4f(pow(sum.rgb,vec3f(1/u.b.y)),sum.a); }
  if (u.b.z<0.5) { sum.a=center.a; }
  return sum;
}
@fragment fn gaussian(v: Quad) -> @location(0) vec4f { return gaussianAt(v.uv); }
@fragment fn glowY(v: Quad) -> @location(0) vec4f {
  let c=gaussianAt(v.uv); if(u.a.x<0.00001) {return c;}
  return clamp(vec4f(c.rgb*u.b.w,c.a),vec4f(0),vec4f(1));
}
fn thresholdS(v: f32) -> f32 {
  if(v<=u.a.y || v>u.a.z) {return 0;} return (v-u.a.y)/(1-u.a.y);
}
fn thresholdD(v: f32) -> f32 {
  if(v<=u.a.y) {return ((u.a.w*v)*v)/max(u.a.y,0.00001);}
  if(v>u.a.z) {return u.a.w*((v*v-u.a.z*v)+u.a.z);} return v;
}
@fragment fn threshold(v: Quad) -> @location(0) vec4f {
  var c=textureSampleLevel(source,linearSampler,v.uv,0);
  if(u.a.x<0.5) {
    let retained=vec3f(thresholdS(c.r),thresholdS(c.g),thresholdS(c.b));
    c=vec4f(c.rgb*((retained.r+retained.g+retained.b)/max(c.r+c.g+c.b,0.00001)),c.a);
  } else {
    let d=vec3f(thresholdD(c.r),thresholdD(c.g),thresholdD(c.b));
    let luminance=dot(d,vec3f(0.2125999927520752,0.7152000069618225,0.0722000002861023));
    c=vec4f(mix(d,vec3f(luminance),u.b.x),c.a);
  } return c;
}
@fragment fn glowBlend(v: Quad) -> @location(0) vec4f {
  let input=textureSampleLevel(source,linearSampler,v.uv,0); var glow=vec4f(0);
  if(u.a.x>0.00001) {glow=textureSampleLevel(auxiliary,linearSampler,v.uv,0);}
  glow=vec4f(glow.rgb*u.b.rgb,glow.a);
  if(u.a.y>0.5) {return clamp(glow,vec4f(0),vec4f(1));}
  return clamp(glow+input-glow*input,vec4f(0),vec4f(1));
}
@fragment fn copy(v: Quad) -> @location(0) vec4f {return textureSampleLevel(source,linearSampler,v.uv,0);}
// Solid fill/stroke subset of MetalTextSdfMaterial::materialMask; full-canvas
// output-space distance field. Per-glyph native atlas/material mapping is not
// yet represented by this experimental adapter.
@fragment fn material(v: Quad) -> @location(0) vec4f {
  let rg=textureSampleLevel(source,linearSampler,v.uv,0).rg; let d=dot(rg,vec2f(1.0/256.0,1));
  let smoothWidth=max(fwidth(d)*u.a.z,0.00001); let outer=max(0.05,0.5-u.a.y*0.5/(2*u.a.x));
  let fill=clamp((d-(0.5-smoothWidth))/(2*smoothWidth),0,1);
  let stroke=clamp((d-(outer-smoothWidth))/(2*smoothWidth),0,1)*(1-fill);
  let f=vec4f(u.b.rgb*u.b.a,u.b.a)*fill;
  let s=vec4f(u.c.rgb*u.c.a,u.c.a)*stroke;
  return f+s*(1-f.a);
}
`;
