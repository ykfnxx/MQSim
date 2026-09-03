#ifndef REQUEST_DEPENDENCY_MANAGER_H
#define REQUEST_DEPENDENCY_MANAGER_H

#include <cstdint>
#include <unordered_map>
#include <vector>
#include "Host_IO_Request.h"

namespace Host_Components
{
	class IO_Flow_Trace_Based;

	class Request_Dependency_Manager
	{
	public:
		void Register_request(std::uint64_t request_id, const std::vector<std::uint64_t>& predecessor_ids);
		void Finalize();
		void Admit(Host_IO_Request* request, IO_Flow_Trace_Based* flow);
		void Complete(Host_IO_Request* request);
		void Validate_drained() const;

	private:
		struct Request_Definition
		{
			std::vector<std::uint64_t> predecessor_ids;
			bool completed;
			sim_time_type completion_time;
			std::vector<std::uint64_t> dependents;
		};

		struct Waiting_Request
		{
			Host_IO_Request* request;
			IO_Flow_Trace_Based* flow;
		};

		std::unordered_map<std::uint64_t, Request_Definition> definitions;
		std::unordered_map<std::uint64_t, Waiting_Request> waiting;
		void Visit(std::uint64_t request_id, std::unordered_map<std::uint64_t, unsigned char>& state) const;
	};
}

#endif
