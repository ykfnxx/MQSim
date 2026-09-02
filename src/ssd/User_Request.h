#ifndef USER_REQUEST_H
#define USER_REQUEST_H

#include <string>
#include <list>
#include "SSD_Defs.h"
#include "../sim/Sim_Defs.h"
#include "Host_Interface_Defs.h"
#include "NVM_Transaction.h"

namespace SSD_Components
{
	enum class UserRequestType { READ, WRITE, TRIM };
	struct Trim_Operation
	{
		LPA_type LPA;
		page_status_type Sector_bitmap;

		Trim_Operation(LPA_type lpa, page_status_type sector_bitmap) : LPA(lpa), Sector_bitmap(sector_bitmap) {}
	};
	class NVM_Transaction;
	class User_Request
	{
	public:
		User_Request();
		IO_Flow_Priority_Class::Priority Priority_class;
		io_request_id_type ID;
		LHA_type Start_LBA;

		sim_time_type STAT_InitiationTime;
		sim_time_type STAT_ResponseTime;
		std::list<NVM_Transaction*> Transaction_list;
		std::list<Trim_Operation> Trim_operations;
		unsigned int Pending_trim_operations;
		unsigned int Sectors_serviced_from_cache;

		unsigned int Size_in_byte;
		unsigned int SizeInSectors;
		UserRequestType Type;
		stream_id_type Stream_id;
		bool ToBeIgnored;
		void* IO_command_info;//used to store host I/O command info
		void* Data;
	private:
		static unsigned int lastId;
	};
}

#endif // !USER_REQUEST_H
