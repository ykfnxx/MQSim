#include "Request_Dependency_Manager.h"
#include "IO_Flow_Trace_Based.h"
#include "../sim/Engine.h"
#include <algorithm>

namespace Host_Components
{
	void Request_Dependency_Manager::Register_request(std::uint64_t request_id,
		const std::vector<std::uint64_t>& predecessor_ids)
	{
		if (definitions.find(request_id) != definitions.end()) {
			PRINT_ERROR("Duplicate DWPDSim request id " << request_id)
		}
		for (std::uint64_t predecessor_id : predecessor_ids) {
			if (request_id == predecessor_id) PRINT_ERROR("DWPDSim request " << request_id << " depends on itself")
		}
		Request_Definition definition = {predecessor_ids, false, 0,
			std::vector<std::uint64_t>()};
		definitions.insert(std::make_pair(request_id, definition));
	}

	void Request_Dependency_Manager::Visit(std::uint64_t request_id,
		std::unordered_map<std::uint64_t, unsigned char>& state) const
	{
		if (state[request_id] == 1) PRINT_ERROR("Cycle in DWPDSim dependency graph at request " << request_id)
		if (state[request_id] == 2) return;
		state[request_id] = 1;
		const Request_Definition& definition = definitions.at(request_id);
		for (std::uint64_t predecessor_id : definition.predecessor_ids) Visit(predecessor_id, state);
		state[request_id] = 2;
	}

	void Request_Dependency_Manager::Finalize()
	{
		for (auto& entry : definitions) {
			Request_Definition& definition = entry.second;
			for (std::uint64_t predecessor_id : definition.predecessor_ids) {
				auto predecessor = definitions.find(predecessor_id);
				if (predecessor == definitions.end()) {
					PRINT_ERROR("Missing predecessor " << predecessor_id << " for request " << entry.first)
				}
				predecessor->second.dependents.push_back(entry.first);
			}
		}
		std::unordered_map<std::uint64_t, unsigned char> state;
		for (const auto& entry : definitions) Visit(entry.first, state);
	}

	void Request_Dependency_Manager::Admit(Host_IO_Request* request, IO_Flow_Trace_Based* flow)
	{
		const Request_Definition& definition = definitions.at(request->External_request_id);
		bool ready = true;
		for (std::uint64_t predecessor_id : definition.predecessor_ids) {
			if (!definitions.at(predecessor_id).completed) ready = false;
		}
		if (ready) {
			flow->Submit_dependency_request(request);
			return;
		}
		waiting.insert(std::make_pair(request->External_request_id, Waiting_Request{request, flow}));
	}

	void Request_Dependency_Manager::Complete(Host_IO_Request* request)
	{
		auto current = definitions.find(request->External_request_id);
		if (current == definitions.end()) return;
		current->second.completed = true;
		current->second.completion_time = Simulator->Time();
		request->Completion_time = Simulator->Time();
		request->Owning_dependency_flow->Record_dependency_completion(*request);
		for (std::uint64_t dependent_id : current->second.dependents) {
			auto pending = waiting.find(dependent_id);
			if (pending == waiting.end()) continue;
			bool ready_to_submit = true;
			for (std::uint64_t predecessor_id : definitions.at(dependent_id).predecessor_ids) {
				if (!definitions.at(predecessor_id).completed) ready_to_submit = false;
			}
			if (!ready_to_submit) continue;
			Waiting_Request ready = pending->second;
			waiting.erase(pending);
			ready.flow->Submit_dependency_request(ready.request);
		}
	}

	void Request_Dependency_Manager::Validate_drained() const
	{
		if (!waiting.empty()) {
			std::uint64_t first_blocked = waiting.begin()->first;
			for (const auto& entry : waiting) first_blocked = std::min(first_blocked, entry.first);
			PRINT_ERROR("Simulation ended with " << waiting.size() << " unresolved DWPDSim dependencies; first blocked request " << first_blocked)
		}
		for (const auto& entry : definitions) {
			if (!entry.second.completed) PRINT_ERROR("DWPDSim request did not complete: " << entry.first)
		}
	}
}
