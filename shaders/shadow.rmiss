#version 460
#extension GL_EXT_ray_tracing : enable

layout(location = 0) rayPayloadInEXT bool inShadow;

void main()
{
    inShadow = false;
}

