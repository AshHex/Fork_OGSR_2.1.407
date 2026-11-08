#pragma once

#include "script_export_space.h"
#include "../xr_3da/LightAnimLibrary.h"

class CLAItem;

class ScriptLight
{
protected:
    ref_light m_light;
    CLAItem* m_light_anim;

    bool bShadow;
    bool bHudMode;
    float fBrightness;
    float fRange;
    Fcolor color;
    LPCSTR texture;
    int iType;

    // Volumetric light
    bool bVolEnable;
    float fVolDistance;
    float fVolQuality;
    float fVolIntensity;

public:
    ScriptLight()
    {
        m_light = ::Render->light_create();

        SetType(IRender_Light::POINT);
        SetShadow(false);
        SetRange(10.f);
        SetColor(Fcolor().set(1, 1, 1, 1));
        SetHudMode(false);
        Enable(true);

        SetVolumetric(false);
        SetVolumetricDistance(.5f);
        SetVolumetricIntensity(.5f);
        SetVolumetricQuality(.5f);

        m_light_anim = nullptr;
        fBrightness = 0.f;
        texture = nullptr;
    }

    virtual ~ScriptLight()
    {
        m_light.destroy();
    }

    IC void SetLanim(LPCSTR name)
    {
        m_light_anim = LALib.FindItem(name);
    }

    IC LPCSTR GetLanim() const
    {
        return m_light_anim != nullptr ? *m_light_anim->cName : nullptr;
    }

    IC void SetPosition(Fvector pos)
    {
        m_light->set_position(pos);
    }

    IC void SetPosition(float x, float y, float z)
    {
        m_light->set_position(Fvector{x, y, z});
    }

    IC void SetDirection(Fvector dir)
    {
        m_light->set_rotation(dir, Fvector().set(1, 0, 0));
    }

    IC void SetDirection(float x, float y, float z)
    {
        m_light->set_rotation(
            Fvector{x, y, z},
            Fvector().set(1, 0, 0)
        );
    }

    IC void SetDirection(Fvector dir, Fvector right)
    {
        m_light->set_rotation(dir, right);
    }

    IC void SetCone(float angle)
    {
        m_light->set_cone(angle);
    }

    IC void Enable(bool state)
    {
        m_light->set_active(state);
    }

    IC bool IsEnabled() const
    {
        return m_light->get_active();
    }

    IC void SetHudMode(bool b)
    {
        m_light->set_hud_mode(b);
    }

    IC bool GetHudMode() const
    {
        return m_light->get_hud_mode();
    }

    IC void SetBrightness(float br)
    {
        fBrightness = br;
    }

    IC float GetBrightness() const
    {
        return fBrightness;
    }

    IC bool GetVolumetric() const
    {
        return bVolEnable;
    }

    IC float GetVolumetricDistance() const
    {
        return fVolDistance;
    }

    IC float GetVolumetricIntensity() const
    {
        return fVolIntensity;
    }

    IC float GetVolumetricQuality() const
    {
        return fVolQuality;
    }

    IC bool GetShadow() const
    {
        return bShadow;
    }

    IC float GetRange() const
    {
        return fRange;
    }

    IC Fcolor GetColor() const
    {
        return color;
    }

    IC LPCSTR GetTexture() const
    {
        return texture;
    }

    IC int GetType() const
    {
        return iType;
    }

    IC void SetShadow(bool state)
    {
        m_light->set_shadow(state);
        bShadow = state;
    }

    IC void SetRange(float range)
    {
        m_light->set_range(range);
        fRange = range;
    }

    IC void SetType(int type)
    {
        m_light->set_type((IRender_Light::LT)type);
        iType = type;
    }

    IC void SetTexture(LPCSTR name)
    {
        m_light->set_texture(name);
        texture = name;
    }

    IC void SetVolumetric(bool state)
    {
        m_light->set_volumetric(state);
        bVolEnable = state;

        if (state)
            SetShadow(true);
    }

    IC void SetVolumetricDistance(float val)
    {
        m_light->set_volumetric_distance(val);
        fVolDistance = val;
    }

    IC void SetVolumetricIntensity(float val)
    {
        m_light->set_volumetric_intensity(val);
        fVolIntensity = val;
    }

    IC void SetVolumetricQuality(float val)
    {
        fVolQuality = val;
    }

    IC void SetColor(Fcolor col)
    {
        m_light_anim = nullptr;
        m_light->set_color(col);
        color = col;
    }

    IC void Update()
    {
        if (m_light_anim)
        {
            int frame;

            u32 clr = m_light_anim->CalculateBGR(
                Device.fTimeGlobal,
                frame
            );

            Fcolor fclr;

            fclr.set(
                (float)color_get_B(clr),
                (float)color_get_G(clr),
                (float)color_get_R(clr),
                1.f
            );

            fclr.mul_rgb(fBrightness);

            m_light->set_color(fclr);
        }
    }

    DECLARE_SCRIPT_REGISTER_FUNCTION
};

add_to_type_list(ScriptLight)
#undef script_type_list
#define script_type_list save_type_list(ScriptLight)