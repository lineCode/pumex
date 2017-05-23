#include <pumex/Pumex.h>
#include <pumex/AssetLoaderAssimp.h>
#include <pumex/utils/Shapes.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

//#define DEFERRED_MEASURE_TIME 1

const uint32_t MAX_BONES = 511;
const VkSampleCountFlagBits SAMPLE_COUNT = VK_SAMPLE_COUNT_4_BIT;

struct PositionData
{
  PositionData()
  {
  }
  PositionData(const glm::mat4& p)
    : position{ p }
  {
  }
  glm::mat4 position;
  glm::mat4 bones[MAX_BONES];
  uint32_t  typeID;
  // std430 ?
};

struct MaterialData
{
  glm::vec4 ambient;
  glm::vec4 diffuse;
  glm::vec4 specular;
  float     shininess;
  uint32_t  diffuseTextureIndex  = 0;
  uint32_t  specularTextureIndex = 0;
  uint32_t  normalTextureIndex = 0;

  // two functions that define material parameters according to data from an asset's material 
  void registerProperties(const pumex::Material& material)
  {
    ambient   = material.getProperty("$clr.ambient", glm::vec4(0, 0, 0, 0));
    diffuse   = material.getProperty("$clr.diffuse", glm::vec4(1, 1, 1, 1));
    specular  = material.getProperty("$clr.specular", glm::vec4(0, 0, 0, 0));
    shininess = material.getProperty("$mat.shininess", glm::vec4(0, 0, 0, 0)).r;
  }
  void registerTextures(const std::map<pumex::TextureSemantic::Type, uint32_t>& textureIndices)
  {
    auto it = textureIndices.find(pumex::TextureSemantic::Diffuse);
    diffuseTextureIndex = (it == textureIndices.end()) ? 0 : it->second;
    it = textureIndices.find(pumex::TextureSemantic::Specular);
    specularTextureIndex = (it == textureIndices.end()) ? 0 : it->second;
    it = textureIndices.find(pumex::TextureSemantic::Normals);
    normalTextureIndex = (it == textureIndices.end()) ? 0 : it->second;
  }
};

struct UpdateData
{
  UpdateData()
  {
  }
  glm::vec3                                cameraPosition;
  glm::vec2                                cameraGeographicCoordinates;
  float                                    cameraDistance;

  glm::vec2                                lastMousePos;
  bool                                     leftMouseKeyPressed;
  bool                                     rightMouseKeyPressed;
};

struct RenderData
{
  RenderData()
    : prevCameraDistance{ 1.0f }, cameraDistance{ 1.0f }
  {
  }
  glm::vec3               prevCameraPosition;
  glm::vec2               prevCameraGeographicCoordinates;
  float                   prevCameraDistance;
  glm::vec3               cameraPosition;
  glm::vec2               cameraGeographicCoordinates;
  float                   cameraDistance;

};

struct DeferredApplicationData
{
  DeferredApplicationData(std::shared_ptr<pumex::Viewer> v, const std::string& mName )
    : viewer{v}
  {
    modelName = viewer->getFullFilePath(mName);
  }
  void setup()
  {
    std::vector<pumex::VertexSemantic> requiredSemantic = { { pumex::VertexSemantic::Position, 3 }, { pumex::VertexSemantic::Normal, 3 }, { pumex::VertexSemantic::TexCoord, 3 },{ pumex::VertexSemantic::BoneIndex, 1 },{ pumex::VertexSemantic::BoneWeight, 1 } };
    assetBuffer.registerVertexSemantic(1, requiredSemantic);
    std::vector<pumex::TextureSemantic> textureSemantic = { { pumex::TextureSemantic::Diffuse, 0 },{ pumex::TextureSemantic::Specular, 1 }, { pumex::TextureSemantic::Normals, 2 } };
    textureRegistry = std::make_shared<pumex::TextureRegistryArrayOfTextures>();
    textureRegistry->setTargetTextureTraits(0, pumex::TextureTraits());
    textureRegistry->setTargetTextureTraits(1, pumex::TextureTraits());
    textureRegistry->setTargetTextureTraits(2, pumex::TextureTraits());
    materialSet = std::make_shared<pumex::MaterialSet<MaterialData>>(viewer, textureRegistry, textureSemantic);

    pumex::AssetLoaderAssimp loader;
    std::shared_ptr<pumex::Asset> asset(loader.load(modelName, false, requiredSemantic));
    CHECK_LOG_THROW (asset.get() == nullptr,  "Model not loaded : " << modelName);

    pumex::BoundingBox bbox = pumex::calculateBoundingBox(*asset, 1);

    modelTypeID = assetBuffer.registerType("object", pumex::AssetTypeDefinition(bbox));
    materialSet->registerMaterials(modelTypeID, asset);
    assetBuffer.registerObjectLOD(modelTypeID, asset, pumex::AssetLodDefinition(0.0f, 10000.0f));

    std::shared_ptr<pumex::Asset> squareAsset = std::make_shared<pumex::Asset>();
    pumex::Geometry square;
    square.name = "square";
    square.semantic = requiredSemantic;
    square.materialIndex = 0;
    pumex::addQuad(square, glm::vec3(1.0f, -1.0f, 0.0f), glm::vec3(-2.0f, 0.0, 0.0), glm::vec3(0.0, 2.0f, 0.0));
    squareAsset->geometries.push_back(square);
    pumex::Material groundMaterial;
    groundMaterial.properties["$clr.ambient"] = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);;
    squareAsset->materials.push_back(groundMaterial);
    pumex::Skeleton::Bone bone;
    squareAsset->skeleton.bones.emplace_back(bone);
    squareAsset->skeleton.boneNames.push_back("root");
    squareAsset->skeleton.invBoneNames.insert({ "root", 0 });

    pumex::BoundingBox squareBbox = pumex::calculateBoundingBox(*squareAsset, 1);
    squareTypeID = assetBuffer.registerType("square", pumex::AssetTypeDefinition(squareBbox));
    materialSet->registerMaterials(squareTypeID, squareAsset);
    assetBuffer.registerObjectLOD(squareTypeID, squareAsset, pumex::AssetLodDefinition(0.0f, 10000.0f));

    materialSet->refreshMaterialStructures();

    pipelineCache = std::make_shared<pumex::PipelineCache>();

    std::vector<pumex::DescriptorSetLayoutBinding> gbufferLayoutBindings =
    {
      { 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 1, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 2, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 3, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 4, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 5, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 6, 64, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    gbufferDescriptorSetLayout = std::make_shared<pumex::DescriptorSetLayout>(gbufferLayoutBindings);

    gbufferDescriptorPool = std::make_shared<pumex::DescriptorPool>(2, gbufferLayoutBindings);

    // building gbufferPipeline layout
    gbufferPipelineLayout = std::make_shared<pumex::PipelineLayout>();
    gbufferPipelineLayout->descriptorSetLayouts.push_back(gbufferDescriptorSetLayout);

    gbufferPipeline = std::make_shared<pumex::GraphicsPipeline>(pipelineCache, gbufferPipelineLayout, defaultRenderPass, 0);
    gbufferPipeline->shaderStages =
    {
      { VK_SHADER_STAGE_VERTEX_BIT, std::make_shared<pumex::ShaderModule>(viewer->getFullFilePath("deferred_gbuffers.vert.spv")), "main" },
      { VK_SHADER_STAGE_FRAGMENT_BIT, std::make_shared<pumex::ShaderModule>(viewer->getFullFilePath("deferred_gbuffers.frag.spv")), "main" }
    };
    gbufferPipeline->vertexInput =
    {
      { 0, VK_VERTEX_INPUT_RATE_VERTEX, requiredSemantic }
    };
    gbufferPipeline->blendAttachments =
    {
      { VK_FALSE, 0xF },
      { VK_FALSE, 0xF },
      { VK_FALSE, 0xF }
    };
    gbufferPipeline->rasterizationSamples = SAMPLE_COUNT;
    gbufferPipeline->dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

/**********/
    std::vector<pumex::DescriptorSetLayoutBinding> compositeLayoutBindings =
    {
      { 0, 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 1, 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 2, 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    compositeDescriptorSetLayout = std::make_shared<pumex::DescriptorSetLayout>(compositeLayoutBindings);

    compositeDescriptorPool = std::make_shared<pumex::DescriptorPool>(2, compositeLayoutBindings);

    // building gbufferPipeline layout
    compositePipelineLayout = std::make_shared<pumex::PipelineLayout>();
    compositePipelineLayout->descriptorSetLayouts.push_back(compositeDescriptorSetLayout);

    compositePipeline = std::make_shared<pumex::GraphicsPipeline>(pipelineCache, compositePipelineLayout, defaultRenderPass, 1);
    compositePipeline->shaderStages =
    {
      { VK_SHADER_STAGE_VERTEX_BIT, std::make_shared<pumex::ShaderModule>(viewer->getFullFilePath("deferred_composite.vert.spv")), "main" },
      { VK_SHADER_STAGE_FRAGMENT_BIT, std::make_shared<pumex::ShaderModule>(viewer->getFullFilePath("deferred_composite.frag.spv")), "main" }
    };
    compositePipeline->depthTestEnable  = VK_FALSE;
    compositePipeline->depthWriteEnable = VK_FALSE;

    compositePipeline->vertexInput =
    {
      { 0, VK_VERTEX_INPUT_RATE_VERTEX, requiredSemantic }
    };
    compositePipeline->blendAttachments =
    {
      { VK_FALSE, 0xF }
    };
    compositePipeline->rasterizationSamples = SAMPLE_COUNT;
    compositePipeline->dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

/***********/

    cameraUbo = std::make_shared<pumex::UniformBuffer<pumex::Camera>>();

    input2 = std::make_shared<pumex::InputAttachment>(2);
    input3 = std::make_shared<pumex::InputAttachment>(3);
    input4 = std::make_shared<pumex::InputAttachment>(4);


    std::vector<glm::mat4> globalTransforms = pumex::calculateResetPosition(*asset);
    PositionData modelData;
    std::copy(globalTransforms.begin(), globalTransforms.end(), std::begin(modelData.bones));
    modelData.typeID = modelTypeID;

    positionUbo = std::make_shared<pumex::UniformBuffer<PositionData>>(modelData);

    gbufferDescriptorSet = std::make_shared<pumex::DescriptorSet>(gbufferDescriptorSetLayout, gbufferDescriptorPool);
    gbufferDescriptorSet->setSource(0, cameraUbo);
    gbufferDescriptorSet->setSource(1, positionUbo);
    gbufferDescriptorSet->setSource(2, materialSet->getTypeBufferDescriptorSetSource());
    gbufferDescriptorSet->setSource(3, materialSet->getMaterialVariantBufferDescriptorSetSource());
    gbufferDescriptorSet->setSource(4, materialSet->getMaterialDefinitionBufferDescriptorSetSource());
    gbufferDescriptorSet->setSource(5, textureRegistry->textureSamplerOffsets);
    gbufferDescriptorSet->setSource(6, textureRegistry->getTextureSamplerDescriptorSetSource());

    compositeDescriptorSet = std::make_shared<pumex::DescriptorSet>(compositeDescriptorSetLayout, compositeDescriptorPool);
    compositeDescriptorSet->setSource(0, input2);
    compositeDescriptorSet->setSource(1, input3);
    compositeDescriptorSet->setSource(2, input4);

    updateData.cameraPosition = glm::vec3(0.0f, 0.0f, 0.0f);
    updateData.cameraGeographicCoordinates = glm::vec2(0.0f, 0.0f);
    updateData.cameraDistance = 1.0f;
    updateData.leftMouseKeyPressed = false;
    updateData.rightMouseKeyPressed = false;
  }

  void surfaceSetup(std::shared_ptr<pumex::Surface> surface)
  {
    std::shared_ptr<pumex::Device> deviceSh = surface->device.lock();
    VkDevice vkDevice = deviceSh->device;

    myCmdBuffer[vkDevice] = std::make_shared<pumex::CommandBuffer>(VK_COMMAND_BUFFER_LEVEL_PRIMARY, deviceSh, surface->commandPool, surface->getImageCount());

    cameraUbo->validate(deviceSh);
    positionUbo->validate(deviceSh);
    input2->validate(surface);
    input3->validate(surface);
    input4->validate(surface);

    // loading models
    assetBuffer.validate(deviceSh, true, surface->commandPool, surface->presentationQueue);
    materialSet->validate(deviceSh, surface->commandPool, surface->presentationQueue);

    pipelineCache->validate(deviceSh);

    gbufferDescriptorSetLayout->validate(deviceSh);
    gbufferDescriptorPool->validate(deviceSh);
    gbufferPipelineLayout->validate(deviceSh);
    gbufferPipeline->validate(deviceSh);

    compositeDescriptorSetLayout->validate(deviceSh);
    compositeDescriptorPool->validate(deviceSh);
    compositePipelineLayout->validate(deviceSh);
    compositePipeline->validate(deviceSh);
  }

  void processInput(std::shared_ptr<pumex::Surface> surface)
  {
#if defined(DEFERRED_MEASURE_TIME)
    auto inputStart = pumex::HPClock::now();
#endif
    std::shared_ptr<pumex::Window>  windowSh = surface->window.lock();

    std::vector<pumex::MouseEvent> mouseEvents = windowSh->getMouseEvents();
    glm::vec2 mouseMove = updateData.lastMousePos;
    for (const auto& m : mouseEvents)
    {
      switch (m.type)
      {
      case pumex::MouseEvent::KEY_PRESSED:
        if (m.button == pumex::MouseEvent::LEFT)
          updateData.leftMouseKeyPressed = true;
        if (m.button == pumex::MouseEvent::RIGHT)
          updateData.rightMouseKeyPressed = true;
        mouseMove.x = m.x;
        mouseMove.y = m.y;
        updateData.lastMousePos = mouseMove;
        break;
      case pumex::MouseEvent::KEY_RELEASED:
        if (m.button == pumex::MouseEvent::LEFT)
          updateData.leftMouseKeyPressed = false;
        if (m.button == pumex::MouseEvent::RIGHT)
          updateData.rightMouseKeyPressed = false;
        break;
      case pumex::MouseEvent::MOVE:
        if (updateData.leftMouseKeyPressed || updateData.rightMouseKeyPressed)
        {
          mouseMove.x = m.x;
          mouseMove.y = m.y;
        }
        break;
      }
    }

    uint32_t updateIndex = viewer->getUpdateIndex();
    RenderData& uData = renderData[updateIndex];
    uData.prevCameraGeographicCoordinates = updateData.cameraGeographicCoordinates;
    uData.prevCameraDistance = updateData.cameraDistance;
    uData.prevCameraPosition = updateData.cameraPosition;

    if (updateData.leftMouseKeyPressed)
    {
      updateData.cameraGeographicCoordinates.x -= 100.0f*(mouseMove.x - updateData.lastMousePos.x);
      updateData.cameraGeographicCoordinates.y += 100.0f*(mouseMove.y - updateData.lastMousePos.y);
      while (updateData.cameraGeographicCoordinates.x < -180.0f)
        updateData.cameraGeographicCoordinates.x += 360.0f;
      while (updateData.cameraGeographicCoordinates.x>180.0f)
        updateData.cameraGeographicCoordinates.x -= 360.0f;
      updateData.cameraGeographicCoordinates.y = glm::clamp(updateData.cameraGeographicCoordinates.y, -90.0f, 90.0f);
      updateData.lastMousePos = mouseMove;
    }
    if (updateData.rightMouseKeyPressed)
    {
      updateData.cameraDistance += 10.0f*(updateData.lastMousePos.y - mouseMove.y);
      if (updateData.cameraDistance<0.1f)
        updateData.cameraDistance = 0.1f;
      updateData.lastMousePos = mouseMove;
    }

    glm::vec3 forward = glm::vec3(cos(updateData.cameraGeographicCoordinates.x * 3.1415f / 180.0f), sin(updateData.cameraGeographicCoordinates.x * 3.1415f / 180.0f), 0) * 0.2f;
    glm::vec3 right = glm::vec3(cos((updateData.cameraGeographicCoordinates.x + 90.0f) * 3.1415f / 180.0f), sin((updateData.cameraGeographicCoordinates.x + 90.0f) * 3.1415f / 180.0f), 0) * 0.2f;
    if (windowSh->isKeyPressed('W'))
      updateData.cameraPosition -= forward;
    if (windowSh->isKeyPressed('S'))
      updateData.cameraPosition += forward;
    if (windowSh->isKeyPressed('A'))
      updateData.cameraPosition -= right;
    if (windowSh->isKeyPressed('D'))
      updateData.cameraPosition += right;

    uData.cameraGeographicCoordinates = updateData.cameraGeographicCoordinates;
    uData.cameraDistance = updateData.cameraDistance;
    uData.cameraPosition = updateData.cameraPosition;

#if defined(DEFERRED_MEASURE_TIME)
    auto inputEnd = pumex::HPClock::now();
    inputDuration = pumex::inSeconds(inputEnd - inputStart);
#endif
  }

  void update(double timeSinceStart, double updateStep)
  {
  }

  void prepareCameraForRendering()
  {
    uint32_t renderIndex = viewer->getRenderIndex();
    const RenderData& rData = renderData[renderIndex];

    float deltaTime = pumex::inSeconds(viewer->getRenderTimeDelta());
    float renderTime = pumex::inSeconds(viewer->getUpdateTime() - viewer->getApplicationStartTime()) + deltaTime;

    glm::vec3 relCam
    (
      rData.cameraDistance * cos(rData.cameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(rData.cameraGeographicCoordinates.y * 3.1415f / 180.0f),
      rData.cameraDistance * sin(rData.cameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(rData.cameraGeographicCoordinates.y * 3.1415f / 180.0f),
      rData.cameraDistance * sin(rData.cameraGeographicCoordinates.y * 3.1415f / 180.0f)
    );
    glm::vec3 prevRelCam
    (
      rData.prevCameraDistance * cos(rData.prevCameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(rData.prevCameraGeographicCoordinates.y * 3.1415f / 180.0f),
      rData.prevCameraDistance * sin(rData.prevCameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(rData.prevCameraGeographicCoordinates.y * 3.1415f / 180.0f),
      rData.prevCameraDistance * sin(rData.prevCameraGeographicCoordinates.y * 3.1415f / 180.0f)
    );
    glm::vec3 eye = relCam + rData.cameraPosition;
    glm::vec3 prevEye = prevRelCam + rData.prevCameraPosition;

    glm::vec3 realEye = eye + deltaTime * (eye - prevEye);
    glm::vec3 realCenter = rData.cameraPosition + deltaTime * (rData.cameraPosition - rData.prevCameraPosition);

    glm::mat4 viewMatrix = glm::lookAt(realEye, realCenter, glm::vec3(0, 0, 1));

    pumex::Camera camera = cameraUbo->get();
    camera.setViewMatrix(viewMatrix);
    camera.setObserverPosition(realEye);
    camera.setTimeSinceStart(renderTime);
    cameraUbo->set(camera);
  }

  void prepareModelForRendering()
  {
#if defined(DEFERRED_MEASURE_TIME)
    auto prepareBuffersStart = pumex::HPClock::now();
#endif

    std::shared_ptr<pumex::Asset> assetX = assetBuffer.getAsset(modelTypeID, 0);
    if (assetX->animations.empty())
      return;

    uint32_t renderIndex = viewer->getRenderIndex();
    const RenderData& rData = renderData[renderIndex];

    float deltaTime = pumex::inSeconds(viewer->getRenderTimeDelta());
    float renderTime = pumex::inSeconds(viewer->getUpdateTime() - viewer->getApplicationStartTime()) + deltaTime;


    PositionData positionData;
    pumex::Animation& anim = assetX->animations[0];
    pumex::Skeleton& skel = assetX->skeleton;

    uint32_t numAnimChannels = anim.channels.size();
    uint32_t numSkelBones = skel.bones.size();

    std::vector<uint32_t> boneChannelMapping(numSkelBones);
    for (uint32_t boneIndex = 0; boneIndex < numSkelBones; ++boneIndex)
    {
      auto it = anim.invChannelNames.find(skel.boneNames[boneIndex]);
      boneChannelMapping[boneIndex] = (it != anim.invChannelNames.end()) ? it->second : UINT32_MAX;
    }

    std::vector<glm::mat4> localTransforms(MAX_BONES);
    std::vector<glm::mat4> globalTransforms(MAX_BONES);

    anim.calculateLocalTransforms(renderTime, localTransforms.data(), numAnimChannels);
    uint32_t bcVal = boneChannelMapping[0];
    glm::mat4 localCurrentTransform = (bcVal == UINT32_MAX) ? skel.bones[0].localTransformation : localTransforms[bcVal];
    globalTransforms[0] = skel.invGlobalTransform * localCurrentTransform;
    for (uint32_t boneIndex = 1; boneIndex < numSkelBones; ++boneIndex)
    {
      bcVal = boneChannelMapping[boneIndex];
      localCurrentTransform = (bcVal == UINT32_MAX) ? skel.bones[boneIndex].localTransformation : localTransforms[bcVal];
      globalTransforms[boneIndex] = globalTransforms[skel.bones[boneIndex].parentIndex] * localCurrentTransform;
    }
    for (uint32_t boneIndex = 0; boneIndex < numSkelBones; ++boneIndex)
      positionData.bones[boneIndex] = globalTransforms[boneIndex] * skel.bones[boneIndex].offsetMatrix;

    positionUbo->set(positionData);

#if defined(DEFERRED_MEASURE_TIME)
    auto prepareBuffersEnd = pumex::HPClock::now();
    prepareBuffersDuration = pumex::inSeconds(prepareBuffersEnd - prepareBuffersStart);
#endif

  }

  void draw(std::shared_ptr<pumex::Surface> surface)
  {
    std::shared_ptr<pumex::Device>  deviceSh = surface->device.lock();
    std::shared_ptr<pumex::Window>  windowSh = surface->window.lock();
    VkDevice                        vkDevice = deviceSh->device;

    uint32_t                       renderIndex = surface->viewer.lock()->getRenderIndex();
    const RenderData&              rData = renderData[renderIndex];

    uint32_t renderWidth = surface->swapChainSize.width;
    uint32_t renderHeight = surface->swapChainSize.height;

    pumex::Camera camera = cameraUbo->get();
    camera.setProjectionMatrix(glm::perspective(glm::radians(60.0f), (float)renderWidth / (float)renderHeight, 0.1f, 100000.0f));
    cameraUbo->set(camera);

    cameraUbo->validate(deviceSh);
    positionUbo->validate(deviceSh);
    // preparing descriptor sets
    gbufferDescriptorSet->setActiveIndex(surface->getImageIndex());
    gbufferDescriptorSet->validate(surface);

    compositeDescriptorSet->setActiveIndex(surface->getImageIndex());
    compositeDescriptorSet->validate(surface);

    auto currentCmdBuffer = myCmdBuffer[vkDevice];
    currentCmdBuffer->setActiveIndex(surface->getImageIndex());
    currentCmdBuffer->cmdBegin();

    std::vector<VkClearValue> clearValues = 
    { 
      pumex::makeColorClearValue(glm::vec4(0.3f, 0.3f, 0.3f, 1.0f)), 
      pumex::makeDepthStencilClearValue(1.0f, 0),
      pumex::makeColorClearValue(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)), 
      pumex::makeColorClearValue(glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)), 
      pumex::makeColorClearValue(glm::vec4(0.3f, 0.3f, 0.3f, 1.0f)), 
      pumex::makeColorClearValue(glm::vec4(0.3f, 0.3f, 0.3f, 1.0f)) 
    };
    currentCmdBuffer->cmdBeginRenderPass(defaultRenderPass, surface->getCurrentFrameBuffer(), pumex::makeVkRect2D(0, 0, renderWidth, renderHeight), clearValues);
    currentCmdBuffer->cmdSetViewport(0, { pumex::makeViewport(0, 0, renderWidth, renderHeight, 0.0f, 1.0f) });
    currentCmdBuffer->cmdSetScissor(0, { pumex::makeVkRect2D(0, 0, renderWidth, renderHeight) });

    currentCmdBuffer->cmdBindPipeline(gbufferPipeline);
    currentCmdBuffer->cmdBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, surface->surface, gbufferPipelineLayout, 0, gbufferDescriptorSet);
    assetBuffer.cmdBindVertexIndexBuffer(deviceSh, currentCmdBuffer, 1, 0);
    assetBuffer.cmdDrawObject(deviceSh, currentCmdBuffer, 1, modelTypeID, 0, 5000.0f);

    currentCmdBuffer->cmdNextSubPass(VK_SUBPASS_CONTENTS_INLINE);

    currentCmdBuffer->cmdBindPipeline(compositePipeline);
    currentCmdBuffer->cmdBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, surface->surface, compositePipelineLayout, 0, compositeDescriptorSet);
    assetBuffer.cmdBindVertexIndexBuffer(deviceSh, currentCmdBuffer, 1, 0);
    assetBuffer.cmdDrawObject(deviceSh, currentCmdBuffer, 1, squareTypeID, 0, 5000.0f);

    currentCmdBuffer->cmdEndRenderPass();
    currentCmdBuffer->cmdEnd();
    currentCmdBuffer->queueSubmit(surface->presentationQueue, { surface->imageAvailableSemaphore }, { VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT }, { surface->renderCompleteSemaphore }, VK_NULL_HANDLE);
  }

  void finishFrame(std::shared_ptr<pumex::Viewer> viewer, std::shared_ptr<pumex::Surface> surface)
  {
  }

  std::shared_ptr<pumex::Viewer> viewer;
  std::string modelName;
  uint32_t    modelTypeID;

  uint32_t    squareTypeID;

  UpdateData                                           updateData;
  std::array<RenderData, 3>                            renderData;

  std::shared_ptr<pumex::UniformBuffer<pumex::Camera>> cameraUbo;
  std::shared_ptr<pumex::UniformBuffer<PositionData>> positionUbo;
  std::shared_ptr<pumex::InputAttachment>             input2;
  std::shared_ptr<pumex::InputAttachment>             input3;
  std::shared_ptr<pumex::InputAttachment>             input4;

  pumex::AssetBuffer                                      assetBuffer;
  std::shared_ptr<pumex::TextureRegistryArrayOfTextures>  textureRegistry;
  std::shared_ptr<pumex::MaterialSet<MaterialData>>       materialSet;
  std::shared_ptr<pumex::RenderPass>                      defaultRenderPass;

  std::shared_ptr<pumex::PipelineCache>                   pipelineCache;

  std::shared_ptr<pumex::DescriptorSetLayout>             gbufferDescriptorSetLayout;
  std::shared_ptr<pumex::PipelineLayout>                  gbufferPipelineLayout;
  std::shared_ptr<pumex::GraphicsPipeline>                gbufferPipeline;
  std::shared_ptr<pumex::DescriptorPool>                  gbufferDescriptorPool;
  std::shared_ptr<pumex::DescriptorSet>                   gbufferDescriptorSet;

  std::shared_ptr<pumex::DescriptorSetLayout>             compositeDescriptorSetLayout;
  std::shared_ptr<pumex::PipelineLayout>                  compositePipelineLayout;
  std::shared_ptr<pumex::GraphicsPipeline>                compositePipeline;
  std::shared_ptr<pumex::DescriptorPool>                  compositeDescriptorPool;
  std::shared_ptr<pumex::DescriptorSet>                   compositeDescriptorSet;


  std::unordered_map<VkDevice, std::shared_ptr<pumex::CommandBuffer>> myCmdBuffer;

};

int main( int argc, char * argv[] )
{
  SET_LOG_INFO;
  LOG_INFO << "Deferred rendering" << std::endl;

  const std::vector<std::string> requestDebugLayers = { { "VK_LAYER_LUNARG_standard_validation" } };
  pumex::ViewerTraits viewerTraits{ "pumex viewer", true, requestDebugLayers, 60 };
  viewerTraits.debugReportFlags = VK_DEBUG_REPORT_ERROR_BIT_EXT;// | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT | VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_DEBUG_BIT_EXT;

  std::shared_ptr<pumex::Viewer> viewer;
  try
  {
    viewer = std::make_shared<pumex::Viewer>(viewerTraits);

    std::vector<pumex::QueueTraits> requestQueues = { pumex::QueueTraits{ VK_QUEUE_GRAPHICS_BIT , 0, { 0.75f } } };
    std::vector<const char*> requestDeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    std::shared_ptr<pumex::Device> device = viewer->addDevice(0, requestQueues, requestDeviceExtensions);
    CHECK_LOG_THROW(!device->isValid(), "Cannot create logical device with requested parameters");

    pumex::WindowTraits windowTraits{ 0, 100, 100, 1024, 768, false, "Deferred rendering" };
    std::shared_ptr<pumex::Window> window = pumex::Window::createWindow(windowTraits);

    pumex::SurfaceTraits surfaceTraits{ 3, VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 1, VK_FORMAT_D24_UNORM_S8_UINT, VK_PRESENT_MODE_MAILBOX_KHR, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR };
    surfaceTraits.definePresentationQueue(pumex::QueueTraits{ VK_QUEUE_GRAPHICS_BIT, 0, { 0.75f } });

    std::vector<pumex::AttachmentDefinition> renderPassAttachments = 
    {
      { pumex::AttachmentDefinition::SwapChain, VK_FORMAT_B8G8R8A8_UNORM,      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,                                       VK_IMAGE_ASPECT_COLOR_BIT,                               VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,     VK_ATTACHMENT_STORE_OP_STORE,     VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0 },
      { pumex::AttachmentDefinition::Depth,     VK_FORMAT_D24_UNORM_S8_UINT,   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,                               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, SAMPLE_COUNT,          VK_ATTACHMENT_LOAD_OP_CLEAR,     VK_ATTACHMENT_STORE_OP_STORE,     VK_ATTACHMENT_LOAD_OP_CLEAR,     VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED,       0 },
      { pumex::AttachmentDefinition::Color,     VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,                               SAMPLE_COUNT,          VK_ATTACHMENT_LOAD_OP_CLEAR,     VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,       0 },
      { pumex::AttachmentDefinition::Color,     VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,                               SAMPLE_COUNT,          VK_ATTACHMENT_LOAD_OP_CLEAR,     VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,       0 },
      { pumex::AttachmentDefinition::Color,     VK_FORMAT_B8G8R8A8_UNORM,      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,                               SAMPLE_COUNT,          VK_ATTACHMENT_LOAD_OP_CLEAR,     VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,       0 },
      { pumex::AttachmentDefinition::Color,     VK_FORMAT_B8G8R8A8_UNORM,      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,                                       VK_IMAGE_ASPECT_COLOR_BIT,                               SAMPLE_COUNT,          VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,       0 }
    };
    std::vector<pumex::SubpassDefinition> renderPassSubpasses = 
    {
      {
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        {},
        { { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },{ 3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, { 4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } },
        {},
        { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
        {},
        0
      },
      {
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        { { 2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },{ 3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },{ 4, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } },
        { { 5, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } },
        { { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } },
        { VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
        {},
        0
      }
    };
    std::vector<pumex::SubpassDependencyDefinition> renderPassDependencies = 
    {
      { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_DEPENDENCY_BY_REGION_BIT },
      { 0, 1,                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT },
      { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_DEPENDENCY_BY_REGION_BIT }
    };

    std::shared_ptr<pumex::RenderPass> renderPass = std::make_shared<pumex::RenderPass>(renderPassAttachments, renderPassSubpasses, renderPassDependencies);
    surfaceTraits.setDefaultRenderPass(renderPass);
#if defined(_WIN32)
    viewer->addDefaultDirectory("..\\data\\sponza");
    viewer->addDefaultDirectory("..\\..\\data\\sponza");
#else
    viewer->addDefaultDirectory("../data/sponza");
    viewer->addDefaultDirectory("../../data/sponza");
#endif
    std::shared_ptr<DeferredApplicationData> applicationData = std::make_shared<DeferredApplicationData>(viewer, "sponza.dae");
    applicationData->defaultRenderPass = renderPass;
    applicationData->setup();

    std::shared_ptr<pumex::Surface> surface = viewer->addSurface(window, device, surfaceTraits);
    applicationData->surfaceSetup(surface);

    tbb::flow::continue_node< tbb::flow::continue_msg > update(viewer->updateGraph, [=](tbb::flow::continue_msg)
    {
      applicationData->processInput(surface);
      applicationData->update(pumex::inSeconds(viewer->getUpdateTime() - viewer->getApplicationStartTime()), pumex::inSeconds(viewer->getUpdateDuration()));
    });

    tbb::flow::make_edge(viewer->startUpdateGraph, update);
    tbb::flow::make_edge(update, viewer->endUpdateGraph);

    // Making the render graph.
    // This one is also "single threaded" ( look at the make_edge() calls ), but presents a method of connecting graph nodes.
    // Consider make_edge() in render graph :
    // viewer->startRenderGraph should point to all root nodes.
    // All leaf nodes should point to viewer->endRenderGraph.
    tbb::flow::continue_node< tbb::flow::continue_msg > prepareBuffers(viewer->renderGraph, [=](tbb::flow::continue_msg) { applicationData->prepareCameraForRendering(); applicationData->prepareModelForRendering(); });
    tbb::flow::continue_node< tbb::flow::continue_msg > startSurfaceFrame(viewer->renderGraph, [=](tbb::flow::continue_msg) { surface->beginFrame(); });
    tbb::flow::continue_node< tbb::flow::continue_msg > drawSurfaceFrame(viewer->renderGraph, [=](tbb::flow::continue_msg) { applicationData->draw(surface); });
    tbb::flow::continue_node< tbb::flow::continue_msg > endSurfaceFrame(viewer->renderGraph, [=](tbb::flow::continue_msg) { surface->endFrame(); });
    tbb::flow::continue_node< tbb::flow::continue_msg > endWholeFrame(viewer->renderGraph, [=](tbb::flow::continue_msg) { applicationData->finishFrame(viewer, surface); });

    tbb::flow::make_edge(viewer->startRenderGraph, prepareBuffers);
    tbb::flow::make_edge(prepareBuffers, startSurfaceFrame);
    tbb::flow::make_edge(startSurfaceFrame, drawSurfaceFrame);
    tbb::flow::make_edge(drawSurfaceFrame, endSurfaceFrame);
    tbb::flow::make_edge(endSurfaceFrame, endWholeFrame);
    tbb::flow::make_edge(endWholeFrame, viewer->endRenderGraph);


    viewer->run();
  }
  catch (const std::exception e)
  {
  }
  catch (...)
  {
  }
  viewer->cleanup();
  FLUSH_LOG;
	return 0;
}