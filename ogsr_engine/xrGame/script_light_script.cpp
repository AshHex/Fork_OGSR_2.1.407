#include "stdafx.h"
#include "script_light.h"

using namespace luabind;

#pragma optimize("s", on)

void ScriptLight::script_register(lua_State* L)
{
    module(L)
    [
        class_<ScriptLight>("script_light")
            .def(constructor<>())

            .def(
                "set_position",
                (void (ScriptLight::*)(Fvector))
                &ScriptLight::SetPosition
            )

            .def(
                "set_position",
                (void (ScriptLight::*)(float, float, float))
                &ScriptLight::SetPosition
            )

            .def(
                "set_direction",
                (void (ScriptLight::*)(Fvector))
                &ScriptLight::SetDirection
            )

            .def(
                "set_direction",
                (void (ScriptLight::*)(float, float, float))
                &ScriptLight::SetDirection
            )

            .def(
                "set_direction",
                (void (ScriptLight::*)(Fvector, Fvector))
                &ScriptLight::SetDirection
            )

            .def("set_cone", &ScriptLight::SetCone)
            .def("update", &ScriptLight::Update)

            .property(
                "color",
                &ScriptLight::GetColor,
                &ScriptLight::SetColor
            )

            .property(
                "texture",
                &ScriptLight::GetTexture,
                &ScriptLight::SetTexture
            )

            .property(
                "enabled",
                &ScriptLight::IsEnabled,
                &ScriptLight::Enable
            )

            .property(
                "type",
                &ScriptLight::GetType,
                &ScriptLight::SetType
            )

            .property(
                "range",
                &ScriptLight::GetRange,
                &ScriptLight::SetRange
            )

            .property(
                "shadow",
                &ScriptLight::GetShadow,
                &ScriptLight::SetShadow
            )

            .property(
                "lanim",
                &ScriptLight::GetLanim,
                &ScriptLight::SetLanim
            )

            .property(
                "lanim_brightness",
                &ScriptLight::GetBrightness,
                &ScriptLight::SetBrightness
            )

            .property(
                "volumetric",
                &ScriptLight::GetVolumetric,
                &ScriptLight::SetVolumetric
            )

            .property(
                "volumetric_quality",
                &ScriptLight::GetVolumetricQuality,
                &ScriptLight::SetVolumetricQuality
            )

            .property(
                "volumetric_distance",
                &ScriptLight::GetVolumetricDistance,
                &ScriptLight::SetVolumetricDistance
            )

            .property(
                "volumetric_intensity",
                &ScriptLight::GetVolumetricIntensity,
                &ScriptLight::SetVolumetricIntensity
            )

            .property(
                "hud_mode",
                &ScriptLight::GetHudMode,
                &ScriptLight::SetHudMode
            )
    ];
}