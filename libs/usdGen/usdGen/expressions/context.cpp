#include "usdGen/expressions/context.h"

#include <cstring>

namespace usdGen::expr {
namespace {
constexpr VariableInfo kVars[] = {
 // $value is polymorphic: its concrete type/shape belongs to the consumer.
 {"$value",Variable::Value,ScalarType::Invalid,0,Domain::All},
 {"$frame",Variable::Frame,ScalarType::Float64,1,Domain::All},
 {"$time",Variable::Time,ScalarType::Float64,1,Domain::All},
 {"$index",Variable::Index,ScalarType::Float64,1,Domain::All},
 {"$count",Variable::Count,ScalarType::Float64,1,Domain::All},
 {"$seed",Variable::Seed,ScalarType::Int32,1,Domain::All},
 {"$descId",Variable::DescId,ScalarType::Float64,1,Domain::All},
 {"$primIndex",Variable::PrimIndex,ScalarType::Float64,1,Domain::Primitive|Domain::Point},
 {"$primCount",Variable::PrimCount,ScalarType::Float64,1,Domain::Primitive|Domain::Point},
 {"$idLo",Variable::IdLo,ScalarType::UInt32,1,Domain::Primitive|Domain::Point},
 {"$idHi",Variable::IdHi,ScalarType::UInt32,1,Domain::Primitive|Domain::Point},
 {"$id",Variable::Id,ScalarType::Float64,1,Domain::Primitive|Domain::Point},
 {"$u",Variable::U,ScalarType::Float64,1,Domain::Primitive|Domain::Point},
 {"$v",Variable::V,ScalarType::Float64,1,Domain::Primitive|Domain::Point},
 {"$faceId",Variable::FaceId,ScalarType::Int32,1,Domain::Primitive|Domain::Point},
 {"$patchId",Variable::PatchId,ScalarType::Int32,1,Domain::Primitive|Domain::Point},
 {"$P",Variable::P,ScalarType::Float32,3,Domain::Primitive|Domain::Point},
 {"$Pref",Variable::PRef,ScalarType::Float32,3,Domain::Primitive|Domain::Point},
 {"$rootP",Variable::RootP,ScalarType::Float32,3,Domain::Primitive|Domain::Point},
 {"$rootPref",Variable::RootPRef,ScalarType::Float32,3,Domain::Primitive|Domain::Point},
 {"$N",Variable::N,ScalarType::Float32,3,Domain::Primitive|Domain::Point},
 {"$Nref",Variable::NRef,ScalarType::Float32,3,Domain::Primitive|Domain::Point},
 {"$dPdu",Variable::DPdu,ScalarType::Float32,3,Domain::Primitive|Domain::Point},
 {"$dPdv",Variable::DPdv,ScalarType::Float32,3,Domain::Primitive|Domain::Point},
 {"$dPduref",Variable::DPduRef,ScalarType::Float32,3,Domain::Primitive|Domain::Point},
 {"$dPdvref",Variable::DPdvRef,ScalarType::Float32,3,Domain::Primitive|Domain::Point},
 {"$t",Variable::T,ScalarType::Float64,1,Domain::Primitive|Domain::Point},
 {"$pointIndex",Variable::PointIndex,ScalarType::Float64,1,Domain::Point},
 {"$pointCount",Variable::PointCount,ScalarType::Float64,1,Domain::Point},
 {"$cLength",Variable::CLength,ScalarType::Float32,1,Domain::Primitive|Domain::Point},
 {"$cWidth",Variable::CWidth,ScalarType::Float32,1,Domain::Primitive|Domain::Point},
 {"$Cs",Variable::Cs,ScalarType::Float32,3,Domain::Primitive|Domain::Point},
 {"$As",Variable::As,ScalarType::Float32,1,Domain::Primitive|Domain::Point},
};
constexpr size_t kVarCount = sizeof(kVars) / sizeof(kVars[0]);
uint32_t Bytes(ScalarType t) { switch(t) { case ScalarType::Bool: return 1; case ScalarType::Int32: case ScalarType::UInt32: case ScalarType::Float32: return 4; case ScalarType::Int64: case ScalarType::UInt64: case ScalarType::Float64: return 8; case ScalarType::Float16: return 2; default: return 0; } }
}

const Registry &Registry::Get() { static Registry r; return r; }
const VariableInfo *Registry::Find(const char *name) const noexcept {
    if (!name) return nullptr; for (auto const &v : kVars) if (std::strcmp(v.name,name)==0) return &v; return nullptr;
}
const VariableInfo *Registry::Find(Variable id) const noexcept {
    for (auto const &v : kVars) if (v.id==id) return &v; return nullptr;
}
bool Registry::Validate(const char *name, Domain domain, std::string *d) const {
    auto v=Find(name); if (!v) { if(d)*d=std::string("unknown expression variable ")+ (name?name:"<null>"); return false; }
    if (domain != Domain::Groom && domain != Domain::Primitive && domain != Domain::Point) { if(d)*d="invalid expression evaluation domain"; return false; }
    if (!HasDomain(v->domains,domain)) { if(d)*d=std::string("variable ")+v->name+" unavailable at requested evaluation domain"; return false; } return true;
}
uint32_t DescriptionId(const char *p) noexcept { uint32_t h=2166136261u; if(!p)return h; for(;*p;++p){h^=uint8_t(*p);h*=16777619u;} return h; }
bool ValidateValueShape(ValueView const &v, ValueShape const &e, std::string *d) {
    if(v.scalar!=e.scalar || v.isArray!=e.isArray || v.elementCount!=e.elementCount || v.components!=e.components || v.rows!=e.rows || v.columns!=e.columns) { if(d)*d="expression value shape/type does not match destination"; return false; }
    const uint64_t bytes = uint64_t(Bytes(v.scalar)) * v.components;
    if(!v.data || Bytes(v.scalar)==0 || bytes > UINT32_MAX || (v.strideBytes && v.strideBytes < bytes)) { if(d)*d="expression value buffer is invalid"; return false; } return true;
}
}
