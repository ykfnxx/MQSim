#ifndef HOST_IO_REQUEST_H
#define HOST_IO_REQUEST_H

#include "../ssd/SSD_Defs.h"

namespace Host_Components
{
	class IO_Flow_Trace_Based;
	enum class Host_IO_Request_Type { READ, WRITE, TRIM };
	class Host_IO_Request
	{
	public:
		sim_time_type Arrival_time;//The time that the request has been generated
		sim_time_type Enqueue_time;//The time that the request enqueued into the I/O queue
		LHA_type Start_LBA;
		unsigned int LBA_count;
		Host_IO_Request_Type Type;
		uint16_t IO_queue_info;
		uint16_t Source_flow_id;//Only used in SATA host interface
		bool Has_external_request_id = false;
		std::uint64_t External_request_id = 0;
		sim_time_type Dependency_release_time = 0;
		sim_time_type Completion_time = 0;
		IO_Flow_Trace_Based* Owning_dependency_flow = NULL;
	};
}

#endif // !HOST_IO_REQUEST_H
