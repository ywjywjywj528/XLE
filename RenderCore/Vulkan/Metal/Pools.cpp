// Copyright 2015 XLGAMES Inc.
//
// Distributed under the MIT License (See
// accompanying file "LICENSE" or the website
// http://www.opensource.org/licenses/mit-license.php)

#include "Pools.h"
#include "ObjectFactory.h"
#include "../../Format.h"

namespace RenderCore { namespace Metal_Vulkan
{
	static VkCommandBufferLevel AsBufferLevel(CommandBufferType type)
	{
		switch (type) {
		default:
		case CommandBufferType::Primary: return VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		case CommandBufferType::Secondary: return VK_COMMAND_BUFFER_LEVEL_SECONDARY;
		}
	}

    VulkanSharedPtr<VkCommandBuffer> CommandPool::Allocate(CommandBufferType type)
	{
        #if defined(CHECK_COMMAND_POOL)
            std::unique_lock<std::mutex> guard(_lock, std::try_to_lock);
            if (!guard.owns_lock())
                Throw(::Exceptions::BasicLabel("Bad lock attempt in CommandPool::Allocate. Multiple threads attempting to use the same object."));
        #endif

		VkCommandBufferAllocateInfo cmd = {};
		cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cmd.pNext = nullptr;
		cmd.commandPool = _pool.get();
		cmd.level = AsBufferLevel(type);
		cmd.commandBufferCount = 1;

		VkCommandBuffer rawBuffer = nullptr;
		auto res = vkAllocateCommandBuffers(_device.get(), &cmd, &rawBuffer);
		VulkanSharedPtr<VkCommandBuffer> result(
			rawBuffer,
			[this](VkCommandBuffer buffer) { this->QueueDestroy(buffer); });
		if (res != VK_SUCCESS)
			Throw(VulkanAPIFailure(res, "Failure while creating command buffer"));
		return result;
	}

	void CommandPool::QueueDestroy(VkCommandBuffer buffer)
	{
		auto currentMarker = _gpuTracker ? _gpuTracker->GetProducerMarker() : ~0u;
		if (_markedDestroys.empty() || _markedDestroys.back()._marker != currentMarker) {
			bool success = _markedDestroys.try_emplace_back(MarkedDestroys{currentMarker, 1u});
			assert(success);	// failure means eating our tail
			if (!success)
				Throw(::Exceptions::BasicLabel("Ran out of buffers in command pool"));
		} else {
			++_markedDestroys.back()._pendingCount;
		}
		_pendingDestroys.push_back(buffer);
	}

	void CommandPool::FlushDestroys()
	{
        #if defined(CHECK_COMMAND_POOL)
            std::unique_lock<std::mutex> guard(_lock, std::try_to_lock);
            if (!guard.owns_lock())
                Throw(::Exceptions::BasicLabel("Bad lock attempt in CommandPool::FlushDestroys. Multiple threads attempting to use the same object."));
        #endif

		auto trackerMarker = _gpuTracker ? _gpuTracker->GetConsumerMarker() : ~0u;
		size_t countToDestroy = 0;
		while (!_markedDestroys.empty() && _markedDestroys.front()._marker <= trackerMarker) {
			countToDestroy += _markedDestroys.front()._pendingCount;
			_markedDestroys.pop_front();
		}

		assert(countToDestroy <= _pendingDestroys.size());
		countToDestroy = std::min(countToDestroy, _pendingDestroys.size());

		if (countToDestroy) {
			vkFreeCommandBuffers(
				_device.get(), _pool.get(),
				(uint32_t)countToDestroy, AsPointer(_pendingDestroys.begin()));
			_pendingDestroys.erase(_pendingDestroys.begin(), _pendingDestroys.begin() + countToDestroy);
		}
	}

    CommandPool::CommandPool(CommandPool&& moveFrom) never_throws
    {
        #if defined(CHECK_COMMAND_POOL)
            std::unique_lock<std::mutex> guard(moveFrom._lock, std::try_to_lock);
            if (!guard.owns_lock())
                Throw(::Exceptions::BasicLabel("Bad lock attempt in CommandPool::Allocate. Multiple threads attempting to use the same object."));
        #endif

        _pool = std::move(moveFrom._pool);
        _device = std::move(moveFrom._device);
        _pendingDestroys = std::move(moveFrom._pendingDestroys);
		_markedDestroys = std::move(_markedDestroys);
		_gpuTracker = std::move(moveFrom._gpuTracker);
    }
    
    CommandPool& CommandPool::operator=(CommandPool&& moveFrom) never_throws
    {
        #if defined(CHECK_COMMAND_POOL)
            // note -- locking both mutexes here.
            //  because we're using try_lock(), it should prevent deadlocks
            std::unique_lock<std::mutex> guard(moveFrom._lock, std::try_to_lock);
            if (!guard.owns_lock())
                Throw(::Exceptions::BasicLabel("Bad lock attempt in CommandPool::Allocate. Multiple threads attempting to use the same object."));

            std::unique_lock<std::mutex> guard2(_lock, std::try_to_lock);
            if (!guard2.owns_lock())
                Throw(::Exceptions::BasicLabel("Bad lock attempt in CommandPool::Allocate. Multiple threads attempting to use the same object."));
        #endif

		if (!_pendingDestroys.empty()) {
			vkFreeCommandBuffers(
				_device.get(), _pool.get(),
				(uint32_t)_pendingDestroys.size(), AsPointer(_pendingDestroys.begin()));
			_pendingDestroys.clear();
		}

        _pool = std::move(moveFrom._pool);
        _device = std::move(moveFrom._device);
        _pendingDestroys = std::move(moveFrom._pendingDestroys);
		_markedDestroys = std::move(_markedDestroys); 
		_gpuTracker = std::move(moveFrom._gpuTracker);
        return *this;
    }

	CommandPool::CommandPool(const Metal_Vulkan::ObjectFactory& factory, unsigned queueFamilyIndex, const std::shared_ptr<IAsyncTracker>& tracker)
	: _device(factory.GetDevice())
	, _gpuTracker(tracker)
	{
		_pool = factory.CreateCommandPool(
            queueFamilyIndex, 
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
	}

	CommandPool::CommandPool() {}
	CommandPool::~CommandPool() 
	{
		if (!_pendingDestroys.empty()) {
			vkFreeCommandBuffers(
				_device.get(), _pool.get(),
				(uint32_t)_pendingDestroys.size(), AsPointer(_pendingDestroys.begin()));
			_pendingDestroys.clear();
		}
	}


    void DescriptorPool::Allocate(
        IteratorRange<VulkanUniquePtr<VkDescriptorSet>*> dst,
        IteratorRange<const VkDescriptorSetLayout*> layouts)
    {
        assert(dst.size() == layouts.size());
        assert(dst.size() > 0);

        VkDescriptorSetAllocateInfo desc_alloc_info;
        desc_alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        desc_alloc_info.pNext = nullptr;
        desc_alloc_info.descriptorPool = _pool.get();
        desc_alloc_info.descriptorSetCount = (uint32_t)std::min(dst.size(), layouts.size());
        desc_alloc_info.pSetLayouts = layouts.begin();

        VkDescriptorSet rawDescriptorSets[4] = { nullptr, nullptr, nullptr, nullptr };

        VkResult res;
        if (desc_alloc_info.descriptorSetCount <= dimof(rawDescriptorSets)) {
            res = vkAllocateDescriptorSets(_device.get(), &desc_alloc_info, rawDescriptorSets);
            for (unsigned c=0; c<desc_alloc_info.descriptorSetCount; ++c)
                dst[c] = VulkanUniquePtr<VkDescriptorSet>(
                    rawDescriptorSets[c],
                    [this](VkDescriptorSet set) { this->QueueDestroy(set); });
        } else {
            std::vector<VkDescriptorSet> rawDescriptorsOverflow;
            rawDescriptorsOverflow.resize(desc_alloc_info.descriptorSetCount, nullptr);
            res = vkAllocateDescriptorSets(_device.get(), &desc_alloc_info, AsPointer(rawDescriptorsOverflow.begin()));
            for (unsigned c=0; c<desc_alloc_info.descriptorSetCount; ++c)
                dst[c] = VulkanUniquePtr<VkDescriptorSet>(
                    rawDescriptorsOverflow[c],
                    [this](VkDescriptorSet set) { this->QueueDestroy(set); });
        }

        if (res != VK_SUCCESS)
			Throw(VulkanAPIFailure(res, "Failure while allocating descriptor set")); 
    }

    void DescriptorPool::FlushDestroys()
    {
		if (!_device || !_pool) return;
        
		auto trackerMarker = _gpuTracker ? _gpuTracker->GetConsumerMarker() : ~0u;
		size_t countToDestroy = 0;
		while (!_markedDestroys.empty() && _markedDestroys.front()._marker <= trackerMarker) {
			countToDestroy += _markedDestroys.front()._pendingCount;
			_markedDestroys.pop_front();
		}

		assert(countToDestroy <= _pendingDestroys.size());
		countToDestroy = std::min(countToDestroy, _pendingDestroys.size());

		if (countToDestroy) {
			vkFreeDescriptorSets(
				_device.get(), _pool.get(),
				(uint32_t)countToDestroy, AsPointer(_pendingDestroys.begin()));
			_pendingDestroys.erase(_pendingDestroys.begin(), _pendingDestroys.begin() + countToDestroy);
		}
    }

	void DescriptorPool::QueueDestroy(VkDescriptorSet set)
	{
		auto currentMarker = _gpuTracker ? _gpuTracker->GetProducerMarker() : ~0u;
		if (_markedDestroys.empty() || _markedDestroys.back()._marker != currentMarker) {
			bool success = _markedDestroys.try_emplace_back(MarkedDestroys{ currentMarker, 1u });
			assert(success);	// failure means eating our tail
			if (!success)
				Throw(::Exceptions::BasicLabel("Ran out of buffers in command pool"));
		} else {
			++_markedDestroys.back()._pendingCount;
		}
		_pendingDestroys.push_back(set);
	}

    DescriptorPool::DescriptorPool(const Metal_Vulkan::ObjectFactory& factory, const std::shared_ptr<IAsyncTracker>& tracker)
    : _device(factory.GetDevice())
	, _gpuTracker(tracker)
    {
        VkDescriptorPoolSize type_count[] = 
        {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 256}
        };

        VkDescriptorPoolCreateInfo descriptor_pool = {};
        descriptor_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptor_pool.pNext = nullptr;
        descriptor_pool.maxSets = 1024;
        descriptor_pool.poolSizeCount = dimof(type_count);
        descriptor_pool.pPoolSizes = type_count;
        descriptor_pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

        _pool = factory.CreateDescriptorPool(descriptor_pool);
    }
    DescriptorPool::DescriptorPool() {}
    DescriptorPool::~DescriptorPool() 
    {
		if (!_pendingDestroys.empty()) {
			vkFreeDescriptorSets(
				_device.get(), _pool.get(),
				(uint32_t)_pendingDestroys.size(), AsPointer(_pendingDestroys.begin()));
			_pendingDestroys.clear();
		}
    }

    DescriptorPool::DescriptorPool(DescriptorPool&& moveFrom) never_throws
    : _pool(std::move(moveFrom._pool))
    , _device(std::move(moveFrom._device))
	, _gpuTracker(std::move(moveFrom._gpuTracker))
	, _markedDestroys(std::move(moveFrom._markedDestroys))
    , _pendingDestroys(moveFrom._pendingDestroys)
    {
    }

    DescriptorPool& DescriptorPool::operator=(DescriptorPool&& moveFrom) never_throws
    {
		if (!_pendingDestroys.empty()) {
			vkFreeDescriptorSets(
				_device.get(), _pool.get(),
				(uint32_t)_pendingDestroys.size(), AsPointer(_pendingDestroys.begin()));
			_pendingDestroys.clear();
		}
        _pool = std::move(moveFrom._pool);
        _device = std::move(moveFrom._device);
		_gpuTracker = std::move(moveFrom._gpuTracker);
		_markedDestroys = std::move(moveFrom._markedDestroys);
		_pendingDestroys = std::move(moveFrom._pendingDestroys);
        return *this;
    }

    static ResourcePtr CreateDummyTexture(const ObjectFactory& factory)
    {
        auto desc = CreateDesc(
            BindFlag::ShaderResource, 
            CPUAccess::Write, GPUAccess::Read, 
            TextureDesc::Plain2D(32, 32, Format::R8G8B8A8_UNORM), "DummyTexture");
        uint32 dummyData[32*32];
        std::memset(dummyData, 0, sizeof(dummyData));
        return CreateResource(
            factory, desc, 
            [&dummyData](SubResourceId)
            {
                return SubResourceInitData{dummyData, 32*32*4, TexturePitches{32*4, 32*32*4}};
            });
    }

    static ResourcePtr CreateDummyUAVImage(const ObjectFactory& factory)
    {
        auto desc = CreateDesc(
            BindFlag::UnorderedAccess, 
            0, GPUAccess::Read|GPUAccess::Write, 
            TextureDesc::Plain2D(32, 32, Format::R8G8B8A8_UNORM), "DummyTexture");
        return CreateResource(factory, desc);
    }

    static ResourcePtr CreateDummyUAVBuffer(const ObjectFactory& factory)
    {
        auto desc = CreateDesc(
            BindFlag::StructuredBuffer, 
            0, GPUAccess::Read|GPUAccess::Write, 
            LinearBufferDesc::Create(256, 256), "DummyBuffer");
        return CreateResource(factory, desc);
    }

    DummyResources::DummyResources(const ObjectFactory& factory)
    : _blankTexture(CreateDummyTexture(factory))
    , _blankUAVImageRes(CreateDummyUAVImage(factory))
    , _blankUAVBufferRes(CreateDummyUAVBuffer(factory))
    , _blankSrv(factory, _blankTexture)
    , _blankUavImage(factory, _blankUAVImageRes)
    , _blankUavBuffer(factory, _blankUAVBufferRes)
    , _blankSampler(std::make_unique<SamplerState>())
    {
        uint8 blankData[4096];
        std::memset(blankData, 0, sizeof(blankData));
        _blankBuffer = Buffer(
            factory, 
            CreateDesc(BindFlag::ConstantBuffer, 0, GPUAccess::Read, LinearBufferDesc::Create(sizeof(blankData)), "DummyBuffer"),
            blankData);
    }

    DummyResources::DummyResources() {}
    DummyResources::~DummyResources() {}

    DummyResources::DummyResources(DummyResources&& moveFrom) never_throws
    : _blankTexture(std::move(moveFrom._blankTexture))
    , _blankUAVImageRes(std::move(moveFrom._blankUAVImageRes))
    , _blankUAVBufferRes(std::move(moveFrom._blankUAVBufferRes))
    , _blankSrv(std::move(moveFrom._blankSrv))
    , _blankUavImage(std::move(moveFrom._blankUavImage))
    , _blankUavBuffer(std::move(moveFrom._blankUavBuffer))
    , _blankBuffer(std::move(moveFrom._blankBuffer))
    , _blankSampler(std::move(moveFrom._blankSampler))
    {}

    DummyResources& DummyResources::operator=(DummyResources&& moveFrom) never_throws
    {
        _blankTexture = std::move(moveFrom._blankTexture);
        _blankUAVImageRes = std::move(moveFrom._blankUAVImageRes);
        _blankUAVBufferRes = std::move(moveFrom._blankUAVBufferRes);
        _blankSrv = std::move(moveFrom._blankSrv);
        _blankUavImage = std::move(moveFrom._blankUavImage);
        _blankUavBuffer = std::move(moveFrom._blankUavBuffer);
        _blankBuffer = std::move(moveFrom._blankBuffer);
        _blankSampler = std::move(moveFrom._blankSampler);
        return *this;
    }

    GlobalPools::GlobalPools() {}
    GlobalPools::~GlobalPools() {}

}}