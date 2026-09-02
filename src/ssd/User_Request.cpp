#include "User_Request.h"

namespace SSD_Components
{
	unsigned int User_Request::lastId = 0;

	User_Request::User_Request() : Pending_trim_operations(0), Sectors_serviced_from_cache(0), IO_command_info(NULL), Data(NULL)
	{
		ID = "" + std::to_string(lastId++);
		ToBeIgnored = false;
	}
}
