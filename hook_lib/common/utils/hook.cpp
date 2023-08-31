#include "hook.hpp"

#include <map>

#include "MinHook.hpp"

#include "concurrency.hpp"
#include "string.hpp"
#include "nt.hpp"

#ifdef max
#undef max
#endif

#ifdef min
#undef min
#endif

namespace utils::hook
{
	namespace
	{
		uint8_t* allocate_somewhere_near(const void* base_address, const size_t size)
		{
			size_t offset = 0;
			while (true)
			{
				offset += size;
				auto* target_address = static_cast<const uint8_t*>(base_address) - offset;
				if (is_relatively_far(base_address, target_address))
				{
					return nullptr;
				}

				const auto res = VirtualAlloc(const_cast<uint8_t*>(target_address), size, MEM_RESERVE | MEM_COMMIT,
				                              PAGE_EXECUTE_READWRITE);
				if (res)
				{
					if (is_relatively_far(base_address, target_address))
					{
						VirtualFree(res, 0, MEM_RELEASE);
						return nullptr;
					}

					return static_cast<uint8_t*>(res);
				}
			}
		}

		class memory
		{
		public:
			memory() = default;

			memory(const void* ptr)
				: memory()
			{
				this->length_ = 0x1000;
				this->buffer_ = allocate_somewhere_near(ptr, this->length_);
				if (!this->buffer_)
				{
					throw std::runtime_error("Failed to allocate");
				}
			}

			~memory()
			{
				if (this->buffer_)
				{
					VirtualFree(this->buffer_, 0, MEM_RELEASE);
				}
			}

			memory(memory&& obj) noexcept
				: memory()
			{
				this->operator=(std::move(obj));
			}

			memory& operator=(memory&& obj) noexcept
			{
				if (this != &obj)
				{
					this->~memory();
					this->buffer_ = obj.buffer_;
					this->length_ = obj.length_;
					this->offset_ = obj.offset_;

					obj.buffer_ = nullptr;
					obj.length_ = 0;
					obj.offset_ = 0;
				}

				return *this;
			}

			void* allocate(const size_t length)
			{
				if (!this->buffer_)
				{
					return nullptr;
				}

				if (this->offset_ + length > this->length_)
				{
					return nullptr;
				}

				const auto ptr = this->get_ptr();
				this->offset_ += length;
				return ptr;
			}

			void* get_ptr() const
			{
				return this->buffer_ + this->offset_;
			}

		private:
			uint8_t* buffer_{};
			size_t length_{};
			size_t offset_{};
		};

		void* get_memory_near(const void* address, const size_t size)
		{
			static concurrency::container<std::vector<memory>> memory_container{};

			return memory_container.access<void*>([&](std::vector<memory>& memories)
			{
				for (auto& memory : memories)
				{
					if (!is_relatively_far(address, memory.get_ptr()))
					{
						const auto buffer = memory.allocate(size);
						if (buffer)
						{
							return buffer;
						}
					}
				}

				memories.emplace_back(address);
				return memories.back().allocate(size);
			});
		}

		void* initialize_min_hook()
		{
			static class min_hook_init
			{
			public:
				min_hook_init()
				{
					if (MH_Initialize() != MH_OK)
					{
						throw std::runtime_error("Failed to initialize MinHook");
					}
				}

				~min_hook_init()
				{
					MH_Uninitialize();
				}
			} min_hook_init;
			return &min_hook_init;
		}
	}

	
	detour::detour()
	{
		(void)initialize_min_hook();
	}

	detour::detour(const size_t place, void* target)
		: detour(reinterpret_cast<void*>(place), target)
	{
	}

	detour::detour(void* place, void* target)
		: detour()
	{
		this->create(place, target);
	}

	detour::~detour()
	{
		this->clear();
	}

	void detour::enable()
	{
		MH_EnableHook(this->place_);
		if (!this->moved_data_.empty())
		{
			this->move();
		}
	}

	void detour::disable()
	{
		this->un_move();
		MH_DisableHook(this->place_);
	}

	void detour::create(void* place, void* target)
	{
		this->clear();
		this->place_ = place;

		if (MH_CreateHook(this->place_, target, &this->original_) != MH_OK)
		{
			//throw std::runtime_error(string::va("Unable to create hook at location: %p", this->place_));
		}

		this->enable();
	}

	void detour::create(const size_t place, void* target)
	{
		MH_Initialize();
		this->create(reinterpret_cast<void*>(place), target);
	}

	void detour::clear()
	{
		if (this->place_)
		{
			this->un_move();
			MH_RemoveHook(this->place_);
		}

		this->place_ = nullptr;
		this->original_ = nullptr;
		this->moved_data_ = {};
	}

	void detour::move()
	{
		this->moved_data_ = move_hook(this->place_);
	}

	void* detour::get_place() const
	{
		return this->place_;
	}

	void* detour::get_original() const
	{
		return this->original_;
	}

	void detour::un_move()
	{
		if (!this->moved_data_.empty())
		{
			copy(this->place_, this->moved_data_.data(), this->moved_data_.size());
		}
	}

	std::optional<std::pair<void*, void*>> iat(const nt::library& library, const std::string& target_library, const std::string& process, void* stub)
	{
		if (!library.is_valid()) return {};

		auto* const ptr = library.get_iat_entry(target_library, process);
		if (!ptr) return {};

		DWORD protect;
		VirtualProtect(ptr, sizeof(*ptr), PAGE_EXECUTE_READWRITE, &protect);

		std::swap(*ptr, stub);

		VirtualProtect(ptr, sizeof(*ptr), protect, &protect);
		return {{ptr, stub}};
	}

	void nop(void* place, const size_t length)
	{
		DWORD old_protect{};
		VirtualProtect(place, length, PAGE_EXECUTE_READWRITE, &old_protect);

		std::memset(place, 0x90, length);

		VirtualProtect(place, length, old_protect, &old_protect);
		FlushInstructionCache(GetCurrentProcess(), place, length);
	}

	void nop(const size_t place, const size_t length)
	{
		nop(reinterpret_cast<void*>(place), length);
	}

	void copy(void* place, const void* data, const size_t length)
	{
		DWORD old_protect{};
		VirtualProtect(place, length, PAGE_EXECUTE_READWRITE, &old_protect);

		std::memmove(place, data, length);

		VirtualProtect(place, length, old_protect, &old_protect);
		FlushInstructionCache(GetCurrentProcess(), place, length);
	}

	void copy(const size_t place, const void* data, const size_t length)
	{
		copy(reinterpret_cast<void*>(place), data, length);
	}

	void copy_string(void* place, const char* str)
	{
		copy(reinterpret_cast<void*>(place), str, strlen(str) + 1);
	}

	void copy_string(const size_t place, const char* str)
	{
		copy_string(reinterpret_cast<void*>(place), str);
	}

	bool is_relatively_far(const void* pointer, const void* data, const int offset)
	{
		const int64_t diff = size_t(data) - (size_t(pointer) + offset);
		const auto small_diff = int32_t(diff);
		return diff != int64_t(small_diff);
	}

	void call(void* pointer, void* data)
	{
		if (is_relatively_far(pointer, data))
		{
			auto* trampoline = get_memory_near(pointer, 14);
			if (!trampoline)
			{
				throw std::runtime_error("Too far away to create 32bit relative branch");
			}

			call(pointer, trampoline);
			jump(trampoline, data, true, true);
			return;
		}

		uint8_t copy_data[5];
		copy_data[0] = 0xE8;
		*reinterpret_cast<int32_t*>(&copy_data[1]) = int32_t(size_t(data) - (size_t(pointer) + 5));

		auto* patch_pointer = PBYTE(pointer);
		copy(patch_pointer, copy_data, sizeof(copy_data));
	}

	void call(const size_t pointer, void* data)
	{
		return call(reinterpret_cast<void*>(pointer), data);
	}

	void call(const size_t pointer, const size_t data)
	{
		return call(pointer, reinterpret_cast<void*>(data));
	}

	void jump(void* pointer, void* data, const bool use_far, const bool use_safe)
	{
		static const unsigned char jump_data[] = {
			0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xff, 0xe0
		};

		static const unsigned char jump_data_safe[] = {
			0xFF, 0x25, 0x00, 0x00, 0x00, 0x00
		};

		if (!use_far && is_relatively_far(pointer, data))
		{
			auto* trampoline = get_memory_near(pointer, 14);
			if (!trampoline)
			{
				throw std::runtime_error("Too far away to create 32bit relative branch");
			}
			jump(pointer, trampoline, false, false);
			jump(trampoline, data, true, true);
			return;
		}

		auto* patch_pointer = PBYTE(pointer);

		if (use_far)
		{
			if (use_safe)
			{
				uint8_t copy_data[sizeof(jump_data_safe) + sizeof(data)];
				memcpy(copy_data, jump_data_safe, sizeof(jump_data_safe));
				memcpy(copy_data + sizeof(jump_data_safe), &data, sizeof(data));

				copy(patch_pointer, copy_data, sizeof(copy_data));
			}
			else
			{
				uint8_t copy_data[sizeof(jump_data)];
				memcpy(copy_data, jump_data, sizeof(jump_data));
				memcpy(copy_data + 2, &data, sizeof(data));

				copy(patch_pointer, copy_data, sizeof(copy_data));
			}
		}
		else
		{
			uint8_t copy_data[5];
			copy_data[0] = 0xE9;
			*reinterpret_cast<int32_t*>(&copy_data[1]) = int32_t(size_t(data) - (size_t(pointer) + 5));

			copy(patch_pointer, copy_data, sizeof(copy_data));
		}
	}

	void jump(const size_t pointer, void* data, const bool use_far, const bool use_safe)
	{
		return jump(reinterpret_cast<void*>(pointer), data, use_far, use_safe);
	}

	void jump(const size_t pointer, const size_t data, const bool use_far, const bool use_safe)
	{
		return jump(pointer, reinterpret_cast<void*>(data), use_far, use_safe);
	}


	void inject(void* pointer, const void* data)
	{
		if (is_relatively_far(pointer, data, 4))
		{
			throw std::runtime_error("Too far away to create 32bit relative branch");
		}

		set<int32_t>(pointer, int32_t(size_t(data) - (size_t(pointer) + 4)));
	}

	void inject(const size_t pointer, const void* data)
	{
		return inject(reinterpret_cast<void*>(pointer), data);
	}

	std::vector<uint8_t> move_hook(void* pointer)
	{
		std::vector<uint8_t> original_data{};

		auto* data_ptr = static_cast<uint8_t*>(pointer);
		if (data_ptr[0] == 0xE9)
		{
			original_data.resize(6);
			memmove(original_data.data(), pointer, original_data.size());

			auto* target = follow_branch(data_ptr);
			nop(data_ptr, 1);
			jump(data_ptr + 1, target);
		}
		else if (data_ptr[0] == 0xFF && data_ptr[1] == 0x25)
		{
			original_data.resize(15);
			memmove(original_data.data(), pointer, original_data.size());

			copy(data_ptr + 1, data_ptr, 14);
			nop(data_ptr, 1);
		}
		else
		{
			throw std::runtime_error("No branch instruction found");
		}

		return original_data;
	}

	std::vector<uint8_t> move_hook(const size_t pointer)
	{
		return move_hook(reinterpret_cast<void*>(pointer));
	}

	void* follow_branch(void* address)
	{
		auto* const data = static_cast<uint8_t*>(address);
		if (*data != 0xE8 && *data != 0xE9)
		{
			throw std::runtime_error("No branch instruction found");
		}

		return extract<void*>(data + 1);
	}
}
