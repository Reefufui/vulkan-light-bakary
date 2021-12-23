// created in 2021 by Andrey Treefonov https://github.com/Reefufui

#include "scene_manager.hpp"

#include <glm/ext/vector_double3.hpp>
#include <glm/ext/matrix_double4x4.hpp>
#include <glm/ext/quaternion_double.hpp>

#include <iostream>
#include <filesystem>
#include <algorithm>
#include <utility>

namespace vlb {

    Scene_t::Scene_t(std::string& filename)
    {
        std::string err;
        std::string warn;

        std::filesystem::path filePath = filename;
        this->path = filename;
        this->name  = filePath.stem();

        bool loaded{false};
        if (filePath.extension() == ".gltf")
        {
            loaded = loader.LoadASCIIFromFile(&this->model, &err, &warn, filename);
        }
        else if (filePath.extension() == ".glb")
        {
            loaded = loader.LoadBinaryFromFile(&this->model, &err, &warn, filename);
        }
        else
        {
            throw std::runtime_error("Not supported file format");
        }

        if (!warn.empty())
        {
            printf("Warn: %s", warn.c_str());
        }

        if (!err.empty())
        {
            printf("Err: %s", err.c_str());
        }

        if (!loaded)
        {
            throw std::runtime_error("Failed to parse glTF");
        }
    }

    Scene Scene_t::init(Scene_t::InitInfo& info)
    {
        this->device               = info.device;
        this->physicalDevice       = info.physicalDevice;
        this->queue.transfer       = info.transferQueue;
        this->commandPool.transfer = info.transferCommandPool;
        this->queue.graphics       = info.graphicsQueue;
        this->commandPool.graphics = info.graphicsCommandPool;
        this->swapchainImagesCount = info.swapchainImagesCount;

        return shared_from_this();
    }

    auto Scene_t::Primitive_t::getGeometry()
    {
        vk::AccelerationStructureGeometryTrianglesDataKHR data{};
        data
            .setVertexFormat(vk::Format::eR32G32B32Sfloat)
            .setVertexStride(sizeof(Vertex))
            .setMaxVertex(this->vertexCount)
            .setIndexType(vk::IndexType::eUint32)
            .setVertexData(this->vertexBuffer.deviceAddress)
            .setIndexData(this->indexBuffer.deviceAddress);

        vk::AccelerationStructureGeometryKHR geometry{};
        geometry
            .setGeometryType(vk::GeometryTypeKHR::eTriangles)
            .setGeometry({data});

        vk::AccelerationStructureBuildRangeInfoKHR range{};
        range
            .setPrimitiveCount(static_cast<uint32_t>(this->indexCount) / 3)
            .setPrimitiveOffset(0)
            .setFirstVertex(0)
            .setTransformOffset(0);

        return std::make_pair(geometry, range);
    }

    /*
       auto Scene_t::Mesh_t::getGeometry()
       {
       VkTransformMatrixKHR transformMatrix = {
       1.0f, 0.0f, 0.0f, 0.0f,
       0.0f, 1.0f, 0.0f, 0.0f,
       0.0f, 0.0f, 1.0f, 0.0f };

       std::vector<vk::AccelerationStructureInstanceKHR> instances();

       for (const auto primitive : this->primitives)
       {
       vk::AccelerationStructureInstanceKHR instance{};
       instance
       .setTransform(transformMatrix)
       .setInstanceCustomIndex(0) // TODO
       .setMask(0xFF)
       .setInstanceShaderBindingTableRecordOffset(0)
       .setFlags(vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable)
       .setAccelerationStructureReference(this->blas.buffer.deviceAddress);
       }

       Application::Buffer instanceBuffer = toBuffer(std::move(instances));

       vk::AccelerationStructureGeometryInstancesDataKHR data{};
       instancesData
       .setArrayOfPointers(false) // TODO
       .setData(instanceBuffer.deviceAddress);

       vk::AccelerationStructureGeometryKHR geometry{};
       geometry
       .setGeometryType(vk::GeometryTypeKHR::eInstances)
       .setGeometry({data})
       .setFlags(vk::GeometryFlagBitsKHR::eOpaque);

       vk::AccelerationStructureBuildRangeInfoKHR range{};
       range
       .setPrimitiveCount(static_cast<uint32_t>(instances.size()))
       .setPrimitiveOffset(0)
       .setFirstVertex(0)
       .setTransformOffset(0);

       return std::make_pair(geometry, range);
       }
       */

    glm::mat4 Scene_t::Node_t::localMatrix()
    {
        return glm::translate(glm::mat4(1.0f), this->translation) * glm::mat4(this->rotation) * glm::scale(glm::mat4(1.0f), this->scale) * this->matrix;
    }

    glm::mat4 Scene_t::Node_t::getMatrix()
    {
        glm::mat4 matrix = localMatrix();
        Node p = this->parent;
        while (p)
        {
            matrix = p->localMatrix() * matrix;
            p = p->parent;
        }
        return matrix;
    }

    auto Scene_t::loadVertexAttribute(const tinygltf::Primitive& primitive, std::string&& label)
    {
        int byteStride{};
        const float* buffer{};
        int componentType{};
        if (primitive.attributes.find(label) != primitive.attributes.end()) {
            const tinygltf::Accessor &accessor = this->model.accessors[primitive.attributes.find(label)->second];
            const tinygltf::BufferView &bufferView = this->model.bufferViews[accessor.bufferView];
            buffer = reinterpret_cast<const float *>(&(this->model.buffers[bufferView.buffer].data[accessor.byteOffset + bufferView.byteOffset]));
            assert(accessor.ByteStride(bufferView));
            byteStride = accessor.ByteStride(bufferView) / sizeof(float);
            componentType = accessor.componentType;
        }
        return std::make_tuple(buffer, byteStride, componentType);
    }

    auto Scene_t::fetchVertices(const tinygltf::Primitive& primitive)
    {
        uint32_t vertexCount = static_cast<uint32_t>(this->model.accessors[primitive.attributes.find("POSITION")->second].count);
        std::vector<Primitive_t::Vertex> vertices(vertexCount);

        auto[posBuffer, posByteStride, posComponentType] = loadVertexAttribute(primitive, "POSITION");
        auto[nrmBuffer, nrmByteStride, nrmComponentType] = loadVertexAttribute(primitive, "NORMAL");
        auto[uv0Buffer, uv0ByteStride, uv0ComponentType] = loadVertexAttribute(primitive, "TEXCOORD_0");
        auto[uv1Buffer, uv1ByteStride, uv1ComponentType] = loadVertexAttribute(primitive, "TEXCOORD_1");


        for (uint32_t v{}; v < vertexCount; ++v)
        {
            if (posBuffer) vertices[v].position = glm::vec4(glm::make_vec3(                &posBuffer[v * posByteStride]), 1.0f);
            if (nrmBuffer) vertices[v].normal   = glm::normalize(glm::vec3(glm::make_vec3( &nrmBuffer[v * nrmByteStride])));
            if (uv0Buffer) vertices[v].uv0      = glm::make_vec2(                          &uv0Buffer[v * uv0ByteStride]);
            if (uv1Buffer) vertices[v].uv1      = glm::make_vec2(                          &uv1Buffer[v * uv1ByteStride]);

            vertices[v].position.y = - vertices[v].position.y;
        }

        return std::move(vertices);
    }

    auto Scene_t::fetchIndices(const tinygltf::Primitive& primitive)
    {
        const auto &accessor   = this->model.accessors[primitive.indices];

        uint32_t indexCount = static_cast<uint32_t>(accessor.count);
        std::vector<uint32_t> indices(indexCount);

        if (indexCount > 0)
        {
            const auto &bufferView = this->model.bufferViews[accessor.bufferView];
            const auto &buffer     = this->model.buffers[bufferView.buffer];

            const void *ptr = &(buffer.data[accessor.byteOffset + bufferView.byteOffset]); // TODO write better :)

            auto fillIndices = [&indices, &ptr, indexCount]<typename T>(const T *buff)
            {
                buff = static_cast<const T*>(ptr);
                for (uint32_t i{}; i < indexCount; ++i)
                {
                    indices[i] = static_cast<uint32_t>(buff[i]);
                }
            };

            if (accessor.componentType == TINYGLTF_PARAMETER_TYPE_UNSIGNED_INT)
            {
                const uint32_t *buff{};
                fillIndices(buff);
            }
            else if (accessor.componentType == TINYGLTF_PARAMETER_TYPE_UNSIGNED_SHORT)
            {
                const uint16_t *buff{};
                fillIndices(buff);
            }
            else if (accessor.componentType == TINYGLTF_PARAMETER_TYPE_UNSIGNED_BYTE)
            {
                const uint8_t *buff{};
                fillIndices(buff);
            }
            else
            {
                throw std::runtime_error("Index component type not supported!");
            }
        }

        return std::move(indices);
    }

    Scene_t::AccelerationStructure Scene_t::buildBLAS(const vk::AccelerationStructureGeometryKHR& geometry, const vk::AccelerationStructureBuildRangeInfoKHR& range)
    {
        vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo
            .setType(vk::AccelerationStructureTypeKHR::eBottomLevel)
            .setFlags(vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace)
            .setGeometries(geometry);

        auto buildSizes = this->device.getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfo, range.primitiveCount);

        using enum vk::BufferUsageFlagBits;
        using enum vk::MemoryPropertyFlagBits;
        vk::BufferUsageFlags    usg{ eAccelerationStructureStorageKHR | eShaderDeviceAddress };
        vk::MemoryPropertyFlags mem{ eHostVisible | eHostCoherent };

        auto blas = std::make_shared<Scene_t::AccelerationStructure_t>();
        blas->buffer = Application::createBuffer(this->device, this->physicalDevice, buildSizes.accelerationStructureSize, usg, mem);
        blas->handle = this->device.createAccelerationStructureKHRUnique(
                vk::AccelerationStructureCreateInfoKHR{}
                .setBuffer(blas->buffer.handle.get())
                .setSize(buildSizes.accelerationStructureSize)
                .setType(vk::AccelerationStructureTypeKHR::eBottomLevel)
                );

        usg = eStorageBuffer | eShaderDeviceAddress;
        mem = eDeviceLocal;
        Application::Buffer scratch = Application::createBuffer(this->device, this->physicalDevice, buildSizes.buildScratchSize, usg, mem);

        buildInfo
            .setMode(vk::BuildAccelerationStructureModeKHR::eBuild)
            .setDstAccelerationStructure(blas->handle.get())
            .setScratchData(scratch.deviceAddress);

        auto cmd = Application::recordCommandBuffer(this->device, this->commandPool.graphics);
        cmd.buildAccelerationStructuresKHR(buildInfo, &range);
        Application::flushCommandBuffer(this->device, this->commandPool.graphics, cmd, this->queue.graphics); // TODO compute

        blas->address = this->device.getAccelerationStructureAddressKHR({blas->handle.get()});

        return std::move(blas);
    }

    Scene_t::AccelerationStructure Scene_t::buildTLAS()
    {
        auto tlas = std::make_shared<Scene_t::AccelerationStructure_t>();

        return tlas;
    }

    Scene Scene_t::buildAccelerationStructures()
    {
        for (auto node : linearNodes)
        {
            auto mesh = node->mesh;

            if (!mesh)
            {
                continue;
            }

            for (auto primitive : mesh->primitives)
            {
                const auto [geometry, range] = primitive->getGeometry();
                primitive->blas = buildBLAS(geometry, range);
                std::cout << "triangles count: " << range.primitiveCount << "\n";
            }
        }

        this->tlas = buildTLAS();

        return shared_from_this();
    }

    void Scene_t::loadNode(const Node parent, const tinygltf::Node& gltfNode, const uint32_t nodeIndex)
    {
        Node node{new Node_t()};
        node->parent = parent;
        node->index  = nodeIndex;

        node->translation = gltfNode.translation.size() != 3 ? glm::dvec3(0.0)
            : glm::make_vec3(gltfNode.translation.data());

        node->rotation = gltfNode.rotation.size() != 3 ? glm::dmat4(glm::dquat{})
            : glm::dmat4(glm::make_quat(gltfNode.rotation.data()));

        node->scale = gltfNode.scale.size() != 3 ? glm::dvec3(1.0f)
            : glm::make_vec3(gltfNode.scale.data());

        node->matrix = gltfNode.matrix.size() != 16 ? glm::dmat4(1.0f)
            : glm::make_mat4x4(gltfNode.matrix.data());

        if (gltfNode.mesh > -1)
        {
            const tinygltf::Mesh& gltfMesh = this->model.meshes[gltfNode.mesh];
            Mesh mesh = std::make_shared<Mesh_t>();

            for (const auto& gltfPrimitive: gltfMesh.primitives)
            {
                auto vertices = fetchVertices(gltfPrimitive);
                auto indices  = fetchIndices(gltfPrimitive);

                Primitive primitive{new Primitive_t()};
                primitive->material     = gltfPrimitive.material > -1 ? this->materials[gltfPrimitive.material] : this->materials.back();
                primitive->vertexCount  = vertices.size();
                primitive->indexCount   = indices.size();
                primitive->vertexBuffer = toBuffer(std::move(vertices));
                primitive->indexBuffer  = toBuffer(std::move(indices));

                mesh->primitives.push_back(std::move(primitive));
            }

            node->mesh = mesh;
        }

        if (parent)
        {
            parent->children.push_back(node);
        }
        else
        {
            nodes.push_back(node);
        }

        linearNodes.push_back(node);

        for (const auto& childIndex : gltfNode.children)
        {
            const auto& child = this->model.nodes[childIndex];
            loadNode(node, child, childIndex);
        }
    }

    Scene Scene_t::loadCameras()
    {
        // TODO: load gltf cameras as well
        Camera camera{new Camera_t()};
        camera
            ->setType(Camera_t::Type::eFirstPerson)
            ->setRotationSpeed(0.2f)
            ->setMovementSpeed(1.0f)
            ->createCameraUBOs(this->device, this->physicalDevice, this->swapchainImagesCount);

        this->cameras.push_back(camera);
        this->cameraIndex = 0;

        return shared_from_this();
    }


    Scene Scene_t::loadSamplers()
    {
        for (auto& gltfSampler : this->model.samplers)
        {
            auto toVkWrapMode = [](int32_t gltfWrapMode) -> vk::SamplerAddressMode
            {
                if (gltfWrapMode == 33071)
                {
                    return vk::SamplerAddressMode::eClampToEdge;
                }
                else if (gltfWrapMode == 33648)
                {
                    return vk::SamplerAddressMode::eMirroredRepeat;
                }
                else
                {
                    return vk::SamplerAddressMode::eRepeat;
                }
            };

            auto toVkFilterMode = [](int32_t gltfFilterMode) -> vk::Filter
            {
                if (gltfFilterMode == 9728 || gltfFilterMode == 9984 || gltfFilterMode == 9985)
                {
                    return vk::Filter::eNearest;
                }
                else
                {
                    return vk::Filter::eLinear;
                }
            };

            Sampler sampler{};
            sampler.minFilter = toVkFilterMode(gltfSampler.minFilter);
            sampler.magFilter = toVkFilterMode(gltfSampler.magFilter);
            sampler.addressModeU = toVkWrapMode(gltfSampler.wrapS);
            sampler.addressModeV = toVkWrapMode(gltfSampler.wrapT);
            sampler.addressModeW = sampler.addressModeV;

            this->samplers.push_back(sampler);
        }

        return shared_from_this();
    }

    Scene Scene_t::loadMaterials()
    {
        auto getTexture = [this](tinygltf::Material& material, std::string&& label)
        {
            if (material.values.find(label) != material.values.end())
            {
                auto& value = material.values[label];

                Texture texture      = this->textures[value.TextureIndex()];
                uint8_t textureCoord = value.TextureTexCoord();

                return std::make_tuple(texture, textureCoord);
            }
            else
            {
                return std::make_tuple(Texture(nullptr), uint8_t(-1));
            }
        };

        for (tinygltf::Material &gltfMaterial : this->model.materials)
        {
            Material material{};

            if (gltfMaterial.additionalValues.find("alphaMode") != gltfMaterial.additionalValues.end())
            {
                tinygltf::Parameter& param = gltfMaterial.additionalValues["alphaMode"];

                if (param.string_value == "BLEND")
                {
                    material.alpha.mode = Material::Alpha::Mode::eBlend;
                }
                else if (param.string_value == "MASK")
                {
                    material.alpha.cutOff = 0.5f;
                    material.alpha.mode = Material::Alpha::Mode::eMask;
                }
                else
                {
                    material.alpha.mode = Material::Alpha::Mode::eOpaque;
                }

                if (gltfMaterial.additionalValues.find("alphaCutoff") != gltfMaterial.additionalValues.end())
                {
                    material.alpha.cutOff = static_cast<float>(gltfMaterial.additionalValues["alphaCutoff"].Factor());
                }
            }

            if (gltfMaterial.values.find("baseColorFactor") != gltfMaterial.values.end())
            {
                material.factor.baseColor = glm::make_vec4(gltfMaterial.values["baseColorFactor"].ColorFactor().data());
            }

            if (gltfMaterial.values.find("metallicFactor") != gltfMaterial.values.end())
            {
                material.factor.metallic = static_cast<float>(gltfMaterial.values["metallicFactor"].Factor());
            }

            if (gltfMaterial.values.find("roughnessFactor") != gltfMaterial.values.end())
            {
                material.factor.roughness = static_cast<float>(gltfMaterial.values["roughnessFactor"].Factor());
            }

            if (gltfMaterial.additionalValues.find("emissiveFactor") != gltfMaterial.additionalValues.end())
            {
                material.factor.emissive = glm::vec4(glm::make_vec3(gltfMaterial.additionalValues["emissiveFactor"].ColorFactor().data()), 1.0);
            }

            {
                auto[texture, textureCoord] = getTexture(gltfMaterial, "normalTexture");
                material.texture.normal     = texture;
                material.coordSet.normal = textureCoord;
            }

            {
                auto[texture, textureCoord] = getTexture(gltfMaterial, "occlusionTexture");
                material.texture.occlusion     = texture;
                material.coordSet.occlusion = textureCoord;
            }

            {
                auto[texture, textureCoord] = getTexture(gltfMaterial, "baseColorTexture");
                material.texture.baseColor     = texture;
                material.coordSet.baseColor = textureCoord;
            }

            {
                auto[texture, textureCoord] = getTexture(gltfMaterial, "metallicRoughnessTexture");
                material.texture.metallicRoughness     = texture;
                material.coordSet.metallicRoughness = textureCoord;
            }

            {
                auto[texture, textureCoord] = getTexture(gltfMaterial, "emissiveTexture");
                material.texture.emissive     = texture;
                material.coordSet.emissive = textureCoord;
            }

            if (gltfMaterial.extensions.find("KHR_materials_pbrSpecularGlossiness") != gltfMaterial.extensions.end())
            {
                auto ext = gltfMaterial.extensions.find("KHR_materials_pbrSpecularGlossiness");

                if (ext->second.Has("specularGlossinessTexture"))
                {
                    auto index = ext->second.Get("specularGlossinessTexture").Get("index");
                    material.texture.specularEXT  = this->textures[index.Get<int>()];
                    material.coordSet.specularEXT = ext->second.Get("specularGlossinessTexture").Get("texCoord").Get<int>();
                }

                if (ext->second.Has("diffuseTexture"))
                {
                    auto index = ext->second.Get("diffuseTexture").Get("index");
                    material.texture.diffuseEXT = this->textures[index.Get<int>()];
                    material.coordSet.diffuseEXT = ext->second.Get("diffuseTexture").Get("texCoord").Get<int>();
                }

                if (ext->second.Has("diffuseFactor"))
                {
                    auto factor = ext->second.Get("diffuseFactor");
                    for (uint32_t i = 0; i < factor.ArrayLen(); i++)
                    {
                        auto val = factor.Get(i);
                        material.factor.diffuseEXT[i] = val.IsNumber() ? (float)val.Get<double>() : (float)val.Get<int>();
                    }
                }
                if (ext->second.Has("specularFactor"))
                {
                    auto factor = ext->second.Get("specularFactor");
                    for (uint32_t i = 0; i < factor.ArrayLen(); i++)
                    {
                        auto val = factor.Get(i);
                        material.factor.specularEXT[i] = val.IsNumber() ? (float)val.Get<double>() : (float)val.Get<int>();
                    }
                }
            }

            materials.push_back(material);
        }

        materials.push_back(Material());

        return shared_from_this();
    }


    Scene Scene_t::loadNodes()
    {
        const auto& scene = this->model.scenes[0];

        for (const auto& nodeIndex : scene.nodes)
        {
            const auto& node = this->model.nodes[nodeIndex];
            loadNode(nullptr, node, nodeIndex);
        }

        return shared_from_this();
    }

    Scene Scene_t::setViewingFrustumForCameras(ViewingFrustum frustum)
    {
        for (auto& camera : cameras)
        {
            camera->setViewingFrustum(frustum);
        }

        return shared_from_this();
    }

    Camera Scene_t::getCamera(int index)
    {
        return this->cameras[index == -1 ? this->cameraIndex : index];
    }

    Scene Scene_t::setCameraIndex(int cameraIndex)
    {
        this->cameraIndex = cameraIndex;

        return shared_from_this();
    }

    const int Scene_t::getCameraIndex()
    {
        return this->cameraIndex;
    }

    Scene Scene_t::pushCamera(Camera camera)
    {
        // TODO: change index here
        this->cameras.push_back(camera);

        return shared_from_this();
    }

    const size_t Scene_t::getCamerasCount()
    {
        return this->cameras.size();
    }

    template <class T>
        Application::Buffer Scene_t::toBuffer(T data)
        {
            size_t size = data.size() * sizeof(data.front());

            using enum vk::BufferUsageFlagBits;
            using enum vk::MemoryPropertyFlagBits;
            vk::BufferUsageFlags    usg{};
            vk::MemoryPropertyFlags mem{};

            usg = eTransferSrc;
            mem = eHostVisible | eHostCoherent;
            Application::Buffer staging = Application::createBuffer(this->device, this->physicalDevice, size, usg, mem, data.data());

            usg = eTransferDst | eAccelerationStructureBuildInputReadOnlyKHR | eShaderDeviceAddress | eStorageBuffer;
            mem = eDeviceLocal;
            Application::Buffer buffer = Application::createBuffer(this->device, this->physicalDevice, size, usg, mem);

            vk::BufferCopy copyRegion{};
            copyRegion.setSize(size);

            auto cmd = Application::recordCommandBuffer(this->device, this->commandPool.transfer);
            cmd.copyBuffer(staging.handle.get(), buffer.handle.get(), 1, &copyRegion);
            Application::flushCommandBuffer(this->device, this->commandPool.transfer, cmd, this->queue.transfer);

            return std::move(buffer);
        }

    // graphics queue requred for blitting! ~duh
    Scene Scene_t::loadTextures()
    {
        for (const auto& gltfTexture : this->model.textures)
        {
            Texture texture{new Texture_t()};

            const tinygltf::Image& gltfImage = this->model.images[gltfTexture.source];
            if (gltfImage.component == 3)
            {
                throw std::runtime_error("RGB (not RGBA) textures not allowed!");
            }

            vk::BufferUsageFlags usage             = vk::BufferUsageFlagBits::eTransferSrc;
            vk::MemoryPropertyFlags memoryProperty = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
            Application::Buffer staging = Application::createBuffer(this->device, this->physicalDevice, gltfImage.image.size(), usage, memoryProperty, gltfImage.image.data());

            std::array<uint32_t, 3> queueFamilyIndices = {0, 1, 2}; // TODO: this might fail but ok for now
            vk::Extent3D extent{static_cast<uint32_t>(gltfImage.width), static_cast<uint32_t>(gltfImage.height), 1};
            uint32_t mipLevels{static_cast<uint32_t>(floor(log2(std::min(gltfImage.width, gltfImage.height))) + 1.0)};

            texture->image.handle = device.createImageUnique(
                    vk::ImageCreateInfo{}
                    .setImageType(vk::ImageType::e2D)
                    .setFormat(vk::Format::eB8G8R8A8Unorm)
                    .setArrayLayers(1)
                    .setMipLevels(mipLevels)
                    .setSamples(vk::SampleCountFlagBits::e1)
                    .setTiling(vk::ImageTiling::eOptimal)
                    .setUsage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc)
                    .setSharingMode(vk::SharingMode::eConcurrent)
                    .setQueueFamilyIndices(queueFamilyIndices)
                    .setInitialLayout(vk::ImageLayout::eUndefined)
                    .setExtent(extent)
                    );

            auto memoryRequirements = this->device.getImageMemoryRequirements(texture->image.handle.get());
            memoryProperty = vk::MemoryPropertyFlagBits::eDeviceLocal;

            texture->image.memory = this->device.allocateMemoryUnique(
                    vk::MemoryAllocateInfo{}
                    .setAllocationSize(memoryRequirements.size)
                    .setMemoryTypeIndex(Application::getMemoryType(this->physicalDevice, memoryRequirements, memoryProperty))
                    );

            device.bindImageMemory(texture->image.handle.get(), texture->image.memory.get(), 0);

            auto blittingCmdBuffer = Application::recordCommandBuffer(this->device, this->commandPool.graphics);

            Application::setImageLayout(blittingCmdBuffer, texture->image.handle.get(), vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                    { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });

            blittingCmdBuffer.copyBufferToImage(
                    staging.handle.get(),
                    texture->image.handle.get(),
                    vk::ImageLayout::eTransferDstOptimal,
                    vk::BufferImageCopy{}
                    .setImageSubresource({ vk::ImageAspectFlagBits::eColor, 0, 0, 1 })
                    .setImageExtent(extent)
                    );

            Application::setImageLayout(blittingCmdBuffer, texture->image.handle.get(), vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                    { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });

            for (uint32_t lvl = 1; lvl < mipLevels; ++lvl)
            {
                auto getMipLevelOffset = [extent](uint32_t lvl)
                {
                    return vk::Offset3D{static_cast<int32_t>(extent.width >> lvl), static_cast<int32_t>(extent.height >> lvl), 1};
                };

                vk::ImageBlit imageBlit{};
                imageBlit
                    .setSrcSubresource({ vk::ImageAspectFlagBits::eColor, lvl - 1, 0, 1 })
                    .setDstSubresource({ vk::ImageAspectFlagBits::eColor, lvl,     0, 1 })
                    .setSrcOffsets({ vk::Offset3D{}, getMipLevelOffset(lvl - 1) })
                    .setDstOffsets({ vk::Offset3D{}, getMipLevelOffset(lvl    ) });

                Application::setImageLayout(blittingCmdBuffer, texture->image.handle.get(), vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                        { vk::ImageAspectFlagBits::eColor, lvl, 1, 0, 1 });

                blittingCmdBuffer.blitImage(texture->image.handle.get(), vk::ImageLayout::eTransferSrcOptimal, texture->image.handle.get(), vk::ImageLayout::eTransferDstOptimal, 1, &imageBlit, vk::Filter::eLinear);

                Application::setImageLayout(blittingCmdBuffer, texture->image.handle.get(), vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                        { vk::ImageAspectFlagBits::eColor, lvl, 1, 0, 1 });
            }

            Application::setImageLayout(blittingCmdBuffer, texture->image.handle.get(), vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    { vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1 });

            Application::flushCommandBuffer(this->device, this->commandPool.graphics, blittingCmdBuffer, queue.graphics);

            Sampler textureSampler = gltfTexture.sampler == -1 ? Sampler{} : this->samplers[gltfTexture.sampler];

            texture->sampler = this->device.createSamplerUnique(
                    vk::SamplerCreateInfo{}
                    .setMagFilter(textureSampler.magFilter)
                    .setMinFilter(textureSampler.minFilter)
                    .setMipmapMode(vk::SamplerMipmapMode::eLinear)
                    .setAddressModeU(textureSampler.addressModeU)
                    .setAddressModeV(textureSampler.addressModeV)
                    .setAddressModeW(textureSampler.addressModeW)
                    .setCompareOp(vk::CompareOp::eNever)
                    .setBorderColor(vk::BorderColor::eFloatOpaqueWhite)
                    .setMaxAnisotropy(1.0)
                    .setAnisotropyEnable(VK_FALSE)
                    .setMaxLod(static_cast<float>(mipLevels))
                    .setMaxAnisotropy(8.0f)
                    .setAnisotropyEnable(VK_TRUE));

            texture->image.imageView = this->device.createImageViewUnique(
                    vk::ImageViewCreateInfo{}
                    .setImage(texture->image.handle.get())
                    .setViewType(vk::ImageViewType::e2D)
                    .setFormat(vk::Format::eB8G8R8A8Unorm)
                    .setComponents({ vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA })
                    .setSubresourceRange({ vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1 })
                    );

            texture->image.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal; // TODO: change layout on go as member

            texture->descriptor
                .setSampler(texture->sampler.get())
                .setImageView(texture->image.imageView.get())
                .setImageLayout(texture->image.imageLayout);

            this->textures.push_back(texture);
        }

        return shared_from_this();
    }

    void SceneManager::pushScene(Scene_t::CreateInfo ci)
    {
        Scene scene{new Scene_t(ci.path)};

        scene->init(this->initInfo);
        scene->loadSamplers();
        scene->loadTextures();
        scene->loadMaterials();
        scene->loadNodes();
        scene->buildAccelerationStructures();

        // Instead of calling loadCameras() to fetch cameras from glTF file we load cameras from ci.
        assert(ci.cameras.size());
        for (auto& cameraInfo : ci.cameras)
        {
            Camera camera{new Camera_t()};
            camera
                ->setType(Camera_t::Type::eFirstPerson)
                ->setRotationSpeed(cameraInfo.rotationSpeed)
                ->setMovementSpeed(cameraInfo.movementSpeed)
                ->setYaw(cameraInfo.yaw)
                ->setPitch(cameraInfo.pitch)
                ->setPosition(cameraInfo.position);
            //TODO->createCameraUBOs(this->device, this->physicalDevice, this->swapchainImagesCount);

            scene->pushCamera(camera);
        }
        scene->setCameraIndex(ci.cameraIndex);
        scene->setViewingFrustumForCameras(this->frustum);

        this->scenes.push_back(scene);
        this->sceneNames.push_back(ci.name);

        this->sceneChangedFlag = false;
    }

    void SceneManager::pushScene(std::string& fileName)
    {
        Scene scene{new Scene_t(fileName)};

        scene->init(this->initInfo);
        scene->loadSamplers();
        scene->loadTextures();
        scene->loadMaterials();
        scene->loadNodes();
        scene->buildAccelerationStructures();
        scene->loadCameras();
        scene->setCameraIndex(0);
        scene->setViewingFrustumForCameras(this->frustum);

        this->scenes.push_back(scene);
        this->sceneNames.push_back(scene->name);

        this->sceneIndex = getScenesCount() - 1;
        this->sceneChangedFlag = true;
    }

    void SceneManager::popScene()
    {
        this->initInfo.device.waitIdle();
        this->scenes.erase(this->scenes.begin() + this->sceneIndex);
        this->sceneNames.erase(this->sceneNames.begin() + this->sceneIndex);

        this->sceneIndex = std::max(0, this->sceneIndex - 1);
        this->sceneChangedFlag = true;
    }

    void SceneManager::init(Scene_t::InitInfo& info)
    {
        initInfo = info;

        this->sceneChangedFlag = false;
        this->sceneIndex = 0;
        this->frustum = std::make_shared<ViewingFrustum_t>();
    }

    Scene& SceneManager::getScene(int index)
    {
        return this->scenes[index == -1 ? this->sceneIndex : index];
    }

    const int SceneManager::getSceneIndex()
    {
        return this->sceneIndex;
    }

    std::vector<std::string>& SceneManager::getSceneNames()
    {
        return this->sceneNames;
    }

    const size_t SceneManager::getScenesCount()
    {
        return this->scenes.size();
    }

    Camera SceneManager::getCamera()
    {
        return getScene()->getCamera();
    }

    SceneManager& SceneManager::setSceneIndex(int sceneIndex)
    {
        this->sceneIndex = sceneIndex;

        static int oldSceneIndex = 0;
        if (this->sceneIndex != oldSceneIndex)
        {
            oldSceneIndex = this->sceneIndex;
            this->sceneChangedFlag = true;
        }

        return *this;
    }

    ViewingFrustum SceneManager::getViewingFrustum()
    {
        return this->frustum;
    }

    SceneManager& SceneManager::setViewingFrustum(ViewingFrustum frustum)
    {
        this->frustum = frustum;
        for (auto& scene : this->scenes)
        {
            scene->setViewingFrustumForCameras(frustum);
        }

        return *this;
    }

    const bool SceneManager::sceneChanged()
    {
        bool sceneChanged = false;
        std::swap(sceneChanged, this->sceneChangedFlag);
        return sceneChanged;
    }

}

