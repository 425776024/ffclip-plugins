// Radial kernel ported from the unchanged MetalRadialBlur.metal. The schedule
// constants (8, 5, 50) come from QtTextRadialBlurMetalRuntimeApple.mm.
export const radial = /* wgsl */ `
fn radialHash(value: vec2f) -> f32 {
  let q0=fract(value*13.517000198364258);
  let q=q0+vec2f(dot(q0,q0.yx+vec2f(22.541000366210938)));
  return fract((q.x+q.y)*q.y);
}
fn radialSample(uv: vec2f) -> vec4f {
  var c=textureSampleLevel(source,linearSampler,uv,0);
  if(u.c.y>0.5){c=vec4f(pow(c.rgb,vec3f(u.c.z)),c.a);} return c;
}
fn radialAt(uvIn: vec2f) -> vec4f {
  var uv=uvIn;
  if(any(uv<vec2f(0))||any(uv>vec2f(1))){
    if(u.b.w<0.5){return vec4f(0);}
    uv=abs(uv); uv=abs(floor(ceil(uv)/2)*2-uv);
  } return radialSample(uv);
}
fn radialRotate(uv: vec2f, angle: f32, aspect: f32) -> vec2f {
  let p=(uv-u.b.xy)*vec2f(1,1/aspect);
  return vec2f(cos(angle)*p.x-sin(angle)*p.y,sin(angle)*p.x+cos(angle)*p.y)*vec2f(1,aspect)+u.b.xy;
}
@fragment fn radialBlur(v: Quad) -> @location(0) vec4f {
  let symmetric=u.a.y==2||u.a.y==4; let intensity=u.a.x*select(1.0,0.5,symmetric);
  let original=radialSample(v.uv); var accumulated=original; var weightTotal=1.0; var weight=1.0;
  let extent0=length((v.uv-u.b.xy)*intensity);
  let extent=sign(extent0)*(0.8999999761581421*abs(extent0)+0.10000000149011612);
  let count=min((8*u.a.z)*extent+5,128.0);
  let weightStep=pow(pow(u.a.w,50.0),1/count);
  let step=((v.uv-u.b.xy)*intensity)/count;
  let jitter=u.b.z*(radialHash(v.uv)*2-1);
  let origin=v.uv+step*jitter;
  let angleStep=6.283185005187988*intensity/count;
  let size=vec2f(textureDimensions(source));
  for(var i=1;i<=128;i++){
    if(f32(i)>count){break;} weight*=weightStep;
    if(u.a.y<3){
      accumulated+=radialAt(origin-step*f32(i))*weight; weightTotal+=weight;
      if(symmetric){accumulated+=radialAt(origin+step*f32(i))*weight;weightTotal+=weight;}
    }else{
      accumulated+=radialAt(radialRotate(v.uv,(f32(i)+jitter)*angleStep,size.x/size.y))*weight;weightTotal+=weight;
      if(symmetric){accumulated+=radialAt(radialRotate(v.uv,(-f32(i)+jitter)*angleStep,size.x/size.y))*weight;weightTotal+=weight;}
    }
  }
  var blurred=accumulated/weightTotal;
  if(u.c.y>0.5){blurred=vec4f(pow(blurred.rgb,vec3f(1/u.c.z)),blurred.a);}
  blurred=vec4f(blurred.rgb*u.c.w,select(original.a,blurred.a,u.c.x>0.5));
  if(u.d.x<0.5){return blurred;} if(u.d.x<1.5){return blurred+original;}
  if(u.d.x<2.5){return max(blurred,original);} return vec4f(1)-(vec4f(1)-blurred)*(vec4f(1)-original);
}
`;

export const sprite = /* wgsl */ `
struct SpriteParams { basis: vec4f, placement: vec4f, output: vec4f };
@group(0) @binding(0) var picture: texture_2d<f32>;
@group(0) @binding(1) var linearSampler: sampler;
@group(0) @binding(2) var<uniform> p: SpriteParams;
struct V { @builtin(position) position: vec4f, @location(0) uv: vec2f };
@vertex fn vertex(@builtin(vertex_index) index: u32) -> V {
  let corners=array<vec2f,6>(vec2f(0,0),vec2f(1,0),vec2f(0,1),vec2f(0,1),vec2f(1,0),vec2f(1,1));
  let uv=corners[index]; let local=uv*p.placement.zw;
  let pixel=vec2f(p.basis.x*local.x+p.basis.z*local.y,p.basis.y*local.x+p.basis.w*local.y)+p.placement.xy;
  var o:V;o.position=vec4f(2*pixel.x/p.output.x-1,1-2*pixel.y/p.output.y,0,1);o.uv=uv;return o;
}
@fragment fn fragment(v:V)->@location(0) vec4f {
  if(p.output.w>0.5){
    let pixel=1.0/vec2f(textureDimensions(picture));
    let uv=clamp(v.uv*vec2f(0.5,1),pixel*0.5,vec2f(0.5,1)-pixel*0.5);
    let a=textureSampleLevel(picture,linearSampler,uv,0).r;
    let color=textureSampleLevel(picture,linearSampler,uv+vec2f(0.5,0),0).rgb;
    // Packed-alpha's right half is already associated RGB.
    return vec4f(color,a)*p.output.z;
  }
  let color=textureSampleLevel(picture,linearSampler,v.uv,0);
  return vec4f(color.rgb*color.a,color.a)*p.output.z;
}
`;
