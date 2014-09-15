#include "Buffer.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

#define INDEX_INITIAL_TOTALPAGECOUNT 0
#define INDEX_INITIAL_NEXTFREEPAGE 1
#define INDEX_INITIAL_AVAILABLEITEMS 2
#define INDEX_INITIAL_AVAILABLEITEMBEGIN 3
#define INDEX_INVALID (~(vuint64_t)0)

namespace vl
{
	namespace database
	{
		using namespace collections;

/***********************************************************************
FileBufferSource
***********************************************************************/

		class FileBufferSource : public Object, public IBufferSource
		{
			typedef collections::Dictionary<vuint64_t, Ptr<BufferPageDesc>>		PageMap;
		private:
			BufferSource			source;
			vuint64_t				pageSize;
			SpinLock				lock;
			WString					fileName;
			int						fileDescriptor;
			PageMap					mappedPages;

		public:

			FileBufferSource(BufferSource _source, vuint64_t _pageSize, const WString& _fileName, int _fileDescriptor)
				:source(_source)
				,pageSize(_pageSize)
				,fileName(_fileName)
				,fileDescriptor(_fileDescriptor)
			{
			}

			void Unload()override
			{
				FOREACH(Ptr<BufferPageDesc>, pageDesc, mappedPages.Values())
				{
					munmap(pageDesc->address, pageSize);
				}
				close(fileDescriptor);
			}

			BufferSource GetBufferSource()override
			{
				return source;
			}

			SpinLock& GetLock()override
			{
				return lock;
			}

			WString GetFileName()override
			{
				return fileName;
			}

			Ptr<BufferPageDesc> MapPage(BufferPage page)
			{
				vint index =mappedPages.Keys().IndexOf(page.index);
				if (index == -1)
				{
					vuint64_t offset = page.index * pageSize;
					struct stat fileState;	
					if (fstat(fileDescriptor, &fileState) == -1)
					{
						return 0;
					}
					if (fileState.st_size < offset + pageSize)
					{
						ftruncate(fileDescriptor, offset + pageSize);
					}

					void* address = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_SHARED, fileDescriptor, offset);
					if (address == MAP_FAILED)
					{
						return nullptr;
					}
					
					auto pageDesc = MakePtr<BufferPageDesc>();
					pageDesc->address = address;
					pageDesc->offset = offset;
					pageDesc->lastAccessTime = (vuint64_t)time(nullptr);
					mappedPages.Add(page.index, pageDesc);
					return pageDesc;
				}
				else
				{
					auto pageDesc = mappedPages.Values()[index];
					pageDesc->lastAccessTime = (vuint64_t)time(nullptr);
					return pageDesc;
				}
			}

			bool UnmapPage(BufferPage page)override
			{
				vint index = mappedPages.Keys().IndexOf(page.index);
				if (index == -1) return false;

				auto pageDesc = mappedPages.Values()[index];
				if (pageDesc->locked) return false;
				munmap(pageDesc->address, pageSize);

				mappedPages.Remove(page.index);
				return true;
			}

			BufferPage AppendPage()
			{
				BufferPage initialPage{0};
				vuint64_t* numbers = (vuint64_t*)LockPage(initialPage);

				BufferPage page{numbers[INDEX_INITIAL_TOTALPAGECOUNT]};
				if (auto pageDesc = MapPage(page))
				{
					numbers[INDEX_INITIAL_TOTALPAGECOUNT]++;
					memset(pageDesc->address, 0, pageSize);
					UnlockPage(initialPage, numbers, true);
				}
				else
				{
					UnlockPage(initialPage, numbers, false);
				}
				return page;
			}

			BufferPage AllocatePage()override
			{
				BufferPage page = BufferPage::Invalid();
				BufferPage freePage{0};
				BufferPage previousFreePage = BufferPage::Invalid();
				while (page.index == INDEX_INVALID && freePage.index != INDEX_INVALID)
				{
					vuint64_t* numbers = (vuint64_t*)LockPage(freePage);
					if (!numbers) return BufferPage::Invalid();

					auto next = numbers[INDEX_INITIAL_NEXTFREEPAGE];
					if (next == INDEX_INVALID)
					{
						vuint64_t& count = numbers[INDEX_INITIAL_AVAILABLEITEMS];
						if (count > 0)
						{
							page.index = numbers[count - 1 + INDEX_INITIAL_AVAILABLEITEMBEGIN];
							count--;
							UnlockPage(freePage, numbers, true);

							if (count == 0 && previousFreePage.index != INDEX_INVALID)
							{
								numbers = (vuint64_t*)LockPage(previousFreePage);
								numbers[INDEX_INITIAL_NEXTFREEPAGE] = INDEX_INVALID;
								UnlockPage(previousFreePage, numbers, true);
								FreePage(freePage);
							}
							break;
						}
					}

					previousFreePage = freePage;
					freePage.index = next;
					UnlockPage(previousFreePage, numbers, false);
				}

				if (page.index == -1)
				{
					return AppendPage();
				}
				else
				{
					return page;
				}
			}

			bool FreePage(BufferPage page)override
			{
				vint index = mappedPages.Keys().IndexOf(page.index);
				if (index != -1)
				{
					auto pageDesc = mappedPages.Values()[index];
					if (pageDesc->locked)
					{
						return false;
					}
				}

				vuint64_t maxAvailableItems = (pageSize - INDEX_INITIAL_AVAILABLEITEMBEGIN * sizeof(vuint64_t)) / sizeof(vuint64_t);
				BufferPage freePage{0};
				while (freePage.index != INDEX_INVALID)
				{
					vuint64_t* numbers = (vuint64_t*)LockPage(freePage);
					if (!numbers)
					{
						return false;
					}

					vuint64_t& count = numbers[INDEX_INITIAL_AVAILABLEITEMS];
					if (count < maxAvailableItems)
					{
						numbers[count + INDEX_INITIAL_AVAILABLEITEMBEGIN] = page.index;
						count++;
						UnlockPage(freePage, numbers, true);
						mappedPages.Remove(page.index);
						return true;
					}
					else if (numbers[INDEX_INITIAL_NEXTFREEPAGE] != INDEX_INVALID)
					{
						vuint64_t index = numbers[INDEX_INITIAL_NEXTFREEPAGE];
						UnlockPage(freePage, numbers, false);
						freePage.index = index;
					}
					else
					{
						BufferPage nextFreePage = AppendPage();
						if (nextFreePage.index != INDEX_INVALID)
						{
							numbers[INDEX_INITIAL_NEXTFREEPAGE] = nextFreePage.index;
							UnlockPage(freePage, numbers, true);

							numbers = (vuint64_t*)LockPage(nextFreePage);
							numbers[INDEX_INITIAL_NEXTFREEPAGE] = INDEX_INVALID;
							numbers[INDEX_INITIAL_AVAILABLEITEMS] = 0;
							UnlockPage(nextFreePage, numbers, true);
							mappedPages.Remove(page.index);
							return true;
						}
						else
						{
							UnlockPage(freePage, numbers, false);
							break;
						}
					}
				}

				return false;
			}

			void* LockPage(BufferPage page)override
			{
				BufferPage initialPage{0};
				if (auto pageDesc = MapPage(initialPage))
				{
					vuint64_t* numbers = (vuint64_t*)pageDesc->address;
					if (page.index >= numbers[INDEX_INITIAL_TOTALPAGECOUNT])
					{
						return nullptr;
					}
				}

				if (auto pageDesc = MapPage(page))
				{
					if (pageDesc->locked) return nullptr;
					pageDesc->locked = true;
					return pageDesc->address;
				}
				else
				{
					return nullptr;
				}
			}

			bool UnlockPage(BufferPage page, void* buffer, bool persist)override
			{
				vint index =mappedPages.Keys().IndexOf(page.index);
				if (index == -1) return false;

				auto pageDesc = mappedPages.Values()[index];
				if (pageDesc->address != buffer) return false;
				if (!pageDesc->locked) return false;

				if (persist)
				{
					msync(pageDesc->address, pageSize, MS_SYNC);
				}
				pageDesc->locked = false;
				return true;
			}	

			void InitializeSource()
			{
				BufferPage page{0};
				auto pageDesc = MapPage(page);
				vuint64_t* numbers = (vuint64_t*)pageDesc->address;
				memset(numbers, 0, pageSize);
				numbers[INDEX_INITIAL_TOTALPAGECOUNT] = 1;
				numbers[INDEX_INITIAL_NEXTFREEPAGE] = INDEX_INVALID;
				numbers[INDEX_INITIAL_AVAILABLEITEMS] = 0;
				msync(numbers, pageSize, MS_SYNC);
			}
		};

		IBufferSource* CreateFileSource(BufferSource source, vuint64_t pageSize, const WString& fileName, bool createNew)
		{
			int fileDescriptor = 0;
			if (createNew)
			{
				auto mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
				fileDescriptor = open(wtoa(fileName).Buffer(), O_CREAT | O_TRUNC | O_RDWR, mode);
			}
			else
			{
				fileDescriptor = open(wtoa(fileName).Buffer(), O_RDWR);
			}

			if (fileDescriptor == -1)
			{
				return nullptr;
			}
			else
			{
				auto result = new FileBufferSource(source, pageSize, fileName, fileDescriptor);
				if (createNew)
				{
					result->InitializeSource();
				}
				return result;
			}
		}
	}
}