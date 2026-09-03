#include "IO_Flow_Trace_Based.h"
#include "../utils/StringTools.h"
#include "ASCII_Trace_Definition.h"
#include "../utils/DistributionTypes.h"
#include <limits>
#include <set>
#include <sstream>

namespace Host_Components
{
IO_Flow_Trace_Based::IO_Flow_Trace_Based(const sim_object_id_type &name, uint16_t flow_id, LHA_type start_lsa_on_device, LHA_type end_lsa_on_device, uint16_t io_queue_id,
										 uint16_t nvme_submission_queue_size, uint16_t nvme_completion_queue_size, IO_Flow_Priority_Class::Priority priority_class, double initial_occupancy_ratio,
										 std::string trace_file_path, Trace_Time_Unit time_unit, unsigned int total_replay_count, unsigned int percentage_to_be_simulated,
										 Trace_Format trace_format, Request_Dependency_Manager* dependency_manager, bool enable_completion_log,
										 HostInterface_Types SSD_device_type, PCIe_Root_Complex *pcie_root_complex, SATA_HBA *sata_hba,
										 bool enabled_logging, sim_time_type logging_period, std::string logging_file_path) : IO_Flow_Base(name, flow_id, start_lsa_on_device, end_lsa_on_device, io_queue_id, nvme_submission_queue_size, nvme_completion_queue_size, priority_class, 0, initial_occupancy_ratio, 0, SSD_device_type, pcie_root_complex, sata_hba, enabled_logging, logging_period, logging_file_path),
																													  trace_file_path(trace_file_path), time_unit(time_unit), total_replay_no(total_replay_count), percentage_to_be_simulated(percentage_to_be_simulated),
																													  total_requests_in_file(0), time_offset(0), trace_format(trace_format), dependency_manager(dependency_manager),
																													  enable_completion_log(enable_completion_log), completion_log_path(logging_file_path + ".completion.csv")
{
	if (percentage_to_be_simulated > 100)
	{
		percentage_to_be_simulated = 100;
		PRINT_MESSAGE("Bad value for percentage of trace file! It is set to 100 % ");
	}
	if (trace_format == Trace_Format::DWPDSIM_DEPENDENCY_V1) Load_dependency_trace();
	Set_dependency_manager(dependency_manager);
}

IO_Flow_Trace_Based::~IO_Flow_Trace_Based()
{
	completion_log.close();
}

Host_IO_Request *IO_Flow_Trace_Based::Generate_next_request()
{
	if (trace_format == Trace_Format::DWPDSIM_DEPENDENCY_V1)
	{
		if (dependency_record_index >= dependency_records.size()) return NULL;
		const Dependency_Trace_Record& record = dependency_records[dependency_record_index];
		Host_IO_Request* request = new Host_IO_Request;
		request->Type = record.type;
		request->LBA_count = record.sector_count;
		request->Start_LBA = start_lsa_on_device + record.start_lba;
		request->Arrival_time = record.arrival_time;
		request->Has_external_request_id = true;
		request->External_request_id = record.request_id;
		request->Owning_dependency_flow = this;
		STAT_generated_request_count++;
		if (record.type == Host_IO_Request_Type::READ) STAT_generated_read_request_count++;
		else if (record.type == Host_IO_Request_Type::WRITE) STAT_generated_write_request_count++;
		else STAT_generated_trim_request_count++;
		return request;
	}
	if (current_trace_line.size() == 0 || STAT_generated_request_count >= total_requests_to_be_generated)
	{
		return NULL;
	}

	Host_IO_Request *request = new Host_IO_Request;
	if (current_trace_line[ASCIITraceTypeColumn].compare(ASCIITraceWriteCode) == 0)
	{
		request->Type = Host_IO_Request_Type::WRITE;
		STAT_generated_write_request_count++;
	}
	else if (current_trace_line[ASCIITraceTypeColumn].compare(ASCIITraceReadCode) == 0)
	{
		request->Type = Host_IO_Request_Type::READ;
		STAT_generated_read_request_count++;
	}
	else if (current_trace_line[ASCIITraceTypeColumn].compare(ASCIITraceTrimCode) == 0)
	{
		request->Type = Host_IO_Request_Type::TRIM;
		STAT_generated_trim_request_count++;
	}
	else
	{
		PRINT_ERROR("Unsupported request type in trace: " << current_trace_line[ASCIITraceTypeColumn])
	}

	char *pEnd;
	request->LBA_count = std::strtoul(current_trace_line[ASCIITraceSizeColumn].c_str(), &pEnd, 0);

	request->Start_LBA = std::strtoull(current_trace_line[ASCIITraceAddressColumn].c_str(), &pEnd, 0);
	if (request->Start_LBA <= (end_lsa_on_device - start_lsa_on_device))
	{
		request->Start_LBA += start_lsa_on_device;
	}
	else
	{
		request->Start_LBA = start_lsa_on_device + request->Start_LBA % (end_lsa_on_device - start_lsa_on_device);
	}

	request->Arrival_time = time_offset + Simulator->Time();
	STAT_generated_request_count++;

	return request;
}

void IO_Flow_Trace_Based::Load_dependency_trace()
{
	if (time_unit != Trace_Time_Unit::NANOSECOND || total_replay_no != 1 || percentage_to_be_simulated != 100) {
		PRINT_ERROR("DWPDSIM_DEPENDENCY_V1 requires nanoseconds, Relay_Count=1 and Percentage_To_Be_Executed=100")
	}
	std::ifstream input(trace_file_path.c_str());
	if (!input.is_open()) PRINT_ERROR("Error while opening input trace file: " << trace_file_path)
	std::string line;
	sim_time_type previous_arrival = 0;
	unsigned int line_number = 0;
	while (std::getline(input, line)) {
		++line_number;
		Utils::Helper_Functions::Remove_cr(line);
		if (line.find_first_not_of(" \t") == std::string::npos) continue;
		std::vector<std::string> columns;
		Utils::Helper_Functions::Tokenize(line, ASCIILineDelimiter, columns);
		if (columns.size() != DWPDSimASCIIItemsPerLine) {
			PRINT_ERROR(trace_file_path << ":" << line_number << ": expected seven DWPDSim trace fields")
		}
		Dependency_Trace_Record record;
		record.arrival_time = std::stoull(columns[ASCIITraceTimeColumn]);
		if (std::stoul(columns[ASCIITraceDeviceColumn]) != 0) {
			PRINT_ERROR(trace_file_path << ":" << line_number << ": DWPDSIM_DEPENDENCY_V1 device_id must be 0")
		}
		record.start_lba = std::stoull(columns[ASCIITraceAddressColumn]);
		record.sector_count = std::stoul(columns[ASCIITraceSizeColumn]);
		if (record.arrival_time < previous_arrival) PRINT_ERROR(trace_file_path << ": arrival times are not monotonic")
		previous_arrival = record.arrival_time;
		if (record.sector_count == 0 || record.sector_count > std::numeric_limits<std::uint16_t>::max()) {
			PRINT_ERROR(trace_file_path << ":" << line_number << ": invalid sector count")
		}
		const LHA_type logical_capacity = end_lsa_on_device - start_lsa_on_device + 1;
		if (record.start_lba >= logical_capacity || record.sector_count > logical_capacity - record.start_lba) {
			PRINT_ERROR(trace_file_path << ":" << line_number << ": request is outside its pool-local namespace")
		}
		const unsigned int operation = std::stoul(columns[ASCIITraceTypeColumn]);
		if (operation == ASCIITraceWriteCodeInteger) record.type = Host_IO_Request_Type::WRITE;
		else if (operation == ASCIITraceReadCodeInteger) record.type = Host_IO_Request_Type::READ;
		else if (operation == ASCIITraceTrimCodeInteger) record.type = Host_IO_Request_Type::TRIM;
		else PRINT_ERROR(trace_file_path << ":" << line_number << ": unsupported operation " << operation)
		record.request_id = std::stoull(columns[DWPDSimTraceRequestIDColumn]);
		std::set<std::uint64_t> predecessor_ids;
		const std::string& predecessor_field = columns[DWPDSimTracePredecessorIDsColumn];
		if (predecessor_field != "-1") {
			std::stringstream input(predecessor_field);
			std::string item;
			while (std::getline(input, item, ',')) {
				if (item.empty()) PRINT_ERROR(trace_file_path << ":" << line_number << ": invalid predecessor list")
				predecessor_ids.insert(std::stoull(item));
			}
		}
		record.predecessor_ids.assign(predecessor_ids.begin(), predecessor_ids.end());
		dependency_records.push_back(record);
		dependency_manager->Register_request(record.request_id, record.predecessor_ids);
	}
	total_requests_in_file = static_cast<unsigned int>(dependency_records.size());
	total_requests_to_be_generated = total_requests_in_file;
}

	void IO_Flow_Trace_Based::Submit_dependency_request(Host_IO_Request* request)
	{
		request->Dependency_release_time = Simulator->Time();
		const sim_time_type wait_time = request->Dependency_release_time - request->Arrival_time;
	Record_dependency_wait(wait_time);
	Submit_io_request(request);
}

void IO_Flow_Trace_Based::Record_dependency_completion(const Host_IO_Request& request)
{
	if (!enable_completion_log) return;
	completion_log << request.External_request_id << ',' << flow_id << ',';
	if (request.Type == Host_IO_Request_Type::READ) completion_log << "READ";
	else if (request.Type == Host_IO_Request_Type::WRITE) completion_log << "WRITE";
	else completion_log << "TRIM";
	completion_log << ',' << request.Arrival_time << ',' << request.Dependency_release_time << ',' << request.Enqueue_time << ','
		<< request.Completion_time << ',' << request.Dependency_release_time - request.Arrival_time << '\n';
}

void IO_Flow_Trace_Based::NVMe_consume_io_request(Completion_Queue_Entry *io_request)
{
	IO_Flow_Base::NVMe_consume_io_request(io_request);
	IO_Flow_Base::NVMe_update_and_submit_completion_queue_tail();
}

void IO_Flow_Trace_Based::SATA_consume_io_request(Host_IO_Request *io_request)
{
	IO_Flow_Base::SATA_consume_io_request(io_request);
}

void IO_Flow_Trace_Based::Start_simulation()
{
	IO_Flow_Base::Start_simulation();
	if (enable_completion_log) {
		completion_log.open(completion_log_path.c_str(), std::ofstream::out);
		completion_log << "request_id,flow_id,operation,arrival_time_ns,dependency_release_time_ns,submit_time_ns,completion_time_ns,dependency_wait_ns\n";
	}
	if (trace_format == Trace_Format::DWPDSIM_DEPENDENCY_V1) {
		if (!dependency_records.empty()) Simulator->Register_sim_event(dependency_records.front().arrival_time, this);
		return;
	}
	std::string trace_line;
	char *pEnd;

	trace_file.open(trace_file_path, std::ios::in);
	if (!trace_file.is_open())
	{
		PRINT_ERROR("Error while opening input trace file: " << trace_file_path)
	}
	PRINT_MESSAGE("Investigating input trace file: " << trace_file_path);

	sim_time_type last_request_arrival_time = 0;
	while (std::getline(trace_file, trace_line))
	{
		Utils::Helper_Functions::Remove_cr(trace_line);
		current_trace_line.clear();
		Utils::Helper_Functions::Tokenize(trace_line, ASCIILineDelimiter, current_trace_line);
		if (current_trace_line.size() != ASCIIItemsPerLine)
		{
			break;
		}
		if (current_trace_line[ASCIITraceTypeColumn].compare(ASCIITraceWriteCode) != 0 &&
			current_trace_line[ASCIITraceTypeColumn].compare(ASCIITraceReadCode) != 0 &&
			current_trace_line[ASCIITraceTypeColumn].compare(ASCIITraceTrimCode) != 0)
		{
			PRINT_ERROR("Unsupported request type in trace: " << current_trace_line[ASCIITraceTypeColumn])
		}
		total_requests_in_file++;
		sim_time_type prev_time = last_request_arrival_time;
		last_request_arrival_time = std::strtoll(current_trace_line[ASCIITraceTimeColumn].c_str(), &pEnd, 10);
		if (last_request_arrival_time < prev_time)
		{
			PRINT_ERROR("Unexpected request arrival time: " << last_request_arrival_time << "\nMQSim expects request arrival times to be monotonically increasing in the input trace!")
		}
	}

	trace_file.close();
	PRINT_MESSAGE("Trace file: " << trace_file_path << " seems healthy");

	if (total_replay_no == 1)
	{
		total_requests_to_be_generated = (int)(((double)percentage_to_be_simulated / 100) * total_requests_in_file);
	}
	else
	{
		total_requests_to_be_generated = total_requests_in_file * total_replay_no;
	}

	trace_file.open(trace_file_path);
	current_trace_line.clear();
	if (!std::getline(trace_file, trace_line)) return;
	Utils::Helper_Functions::Remove_cr(trace_line);
	Utils::Helper_Functions::Tokenize(trace_line, ASCIILineDelimiter, current_trace_line);
	Simulator->Register_sim_event(std::strtoll(current_trace_line[ASCIITraceTimeColumn].c_str(), &pEnd, 10), this);
}

void IO_Flow_Trace_Based::Validate_simulation_config()
{
}

void IO_Flow_Trace_Based::Execute_simulator_event(MQSimEngine::Sim_Event *)
{
	if (trace_format == Trace_Format::DWPDSIM_DEPENDENCY_V1) {
		Host_IO_Request* request = Generate_next_request();
		if (request != NULL) dependency_manager->Admit(request, this);
		++dependency_record_index;
		if (dependency_record_index < dependency_records.size()) {
			Simulator->Register_sim_event(dependency_records[dependency_record_index].arrival_time, this);
		}
		return;
	}
	Host_IO_Request *request = Generate_next_request();
	if (request != NULL)
	{
		Submit_io_request(request);
	}

	if (STAT_generated_request_count < total_requests_to_be_generated)
	{
		std::string trace_line;
		if (std::getline(trace_file, trace_line))
		{
			Utils::Helper_Functions::Remove_cr(trace_line);
			current_trace_line.clear();
			Utils::Helper_Functions::Tokenize(trace_line, ASCIILineDelimiter, current_trace_line);
		}
		else
		{
			trace_file.close();
			trace_file.open(trace_file_path);
			replay_counter++;
			time_offset = Simulator->Time();
			std::getline(trace_file, trace_line);
			Utils::Helper_Functions::Remove_cr(trace_line);
			current_trace_line.clear();
			Utils::Helper_Functions::Tokenize(trace_line, ASCIILineDelimiter, current_trace_line);
			PRINT_MESSAGE("* Replay round " << replay_counter << "of " << total_replay_no << " started  for" << ID())
		}
		char *pEnd;
		Simulator->Register_sim_event(time_offset + std::strtoll(current_trace_line[ASCIITraceTimeColumn].c_str(), &pEnd, 10), this);
	}
}

void IO_Flow_Trace_Based::Get_statistics(Utils::Workload_Statistics &stats, LPA_type (*Convert_host_logical_address_to_device_address)(LHA_type lha),
										 page_status_type (*Find_NVM_subunit_access_bitmap)(LHA_type lha))
{
	if (trace_format == Trace_Format::DWPDSIM_DEPENDENCY_V1) {
		stats.Type = Utils::Workload_Type::TRACE_BASED;
		stats.Stream_id = io_queue_id - 1;
		stats.Min_LHA = start_lsa_on_device;
		stats.Max_LHA = end_lsa_on_device;
		stats.Total_generated_requests = 0;
		stats.Total_accessed_lbas = 0;
		return;
	}
	stats.Type = Utils::Workload_Type::TRACE_BASED;
	stats.Stream_id = io_queue_id - 1; //In MQSim, there is a simple relation between stream id and the io_queue_id of NVMe
	stats.Min_LHA = start_lsa_on_device;
	stats.Max_LHA = end_lsa_on_device;
	for (int i = 0; i < MAX_ARRIVAL_TIME_HISTOGRAM + 1; i++)
	{
		stats.Write_arrival_time.push_back(0);
		stats.Read_arrival_time.push_back(0);
	}
	for (int i = 0; i < MAX_REQSIZE_HISTOGRAM_ITEMS + 1; i++)
	{
		stats.Write_size_histogram.push_back(0);
		stats.Read_size_histogram.push_back(0);
	}
	stats.Total_generated_requests = 0;
	stats.Total_accessed_lbas = 0;

	std::ifstream trace_file_temp;
	trace_file_temp.open(trace_file_path, std::ios::in);
	if (!trace_file_temp.is_open())
	{
		PRINT_ERROR("Error while opening the input trace file!")
	}

	std::string trace_line;
	char *pEnd;
	sim_time_type last_request_arrival_time = 0;
	sim_time_type last_data_request_arrival_time = 0;
	sim_time_type sum_inter_arrival = 0;
	uint64_t sum_request_size = 0;
	std::vector<std::string> line_splitted;
	while (std::getline(trace_file_temp, trace_line))
	{
		Utils::Helper_Functions::Remove_cr(trace_line);
		line_splitted.clear();
		Utils::Helper_Functions::Tokenize(trace_line, ASCIILineDelimiter, line_splitted);
		if (line_splitted.size() != ASCIIItemsPerLine)
		{
			break;
		}
		if (line_splitted[ASCIITraceTypeColumn].compare(ASCIITraceWriteCode) != 0 &&
			line_splitted[ASCIITraceTypeColumn].compare(ASCIITraceReadCode) != 0 &&
			line_splitted[ASCIITraceTypeColumn].compare(ASCIITraceTrimCode) != 0)
		{
			PRINT_ERROR("Unsupported request type in trace: " << line_splitted[ASCIITraceTypeColumn])
		}
		sim_time_type prev_time = last_request_arrival_time;
		last_request_arrival_time = std::strtoull(line_splitted[ASCIITraceTimeColumn].c_str(), &pEnd, 10);
		if (last_request_arrival_time < prev_time)
		{
			PRINT_ERROR("Unexpected request arrival time: " << last_request_arrival_time << "\nMQSim expects request arrival times to be monotonic increasing in the input trace!")
		}
		if (line_splitted[ASCIITraceTypeColumn].compare(ASCIITraceTrimCode) == 0)
		{
			continue; //TRIM does not contribute data-access statistics used by preconditioning.
		}
		sim_time_type diff = (last_request_arrival_time - last_data_request_arrival_time) / 1000; //The arrival rate histogram is stored in the microsecond unit
		sum_inter_arrival += last_request_arrival_time - last_data_request_arrival_time;
		last_data_request_arrival_time = last_request_arrival_time;

		unsigned int LBA_count = std::strtoul(line_splitted[ASCIITraceSizeColumn].c_str(), &pEnd, 0);
		sum_request_size += LBA_count;
		LHA_type start_LBA = std::strtoull(line_splitted[ASCIITraceAddressColumn].c_str(), &pEnd, 0);
		if (start_LBA <= (end_lsa_on_device - start_lsa_on_device))
		{
			start_LBA += start_lsa_on_device;
		}
		else
		{
			start_LBA = start_lsa_on_device + start_LBA % (end_lsa_on_device - start_lsa_on_device);
		}
		LHA_type end_LBA = start_LBA + LBA_count - 1;
		if (end_LBA > end_lsa_on_device)
		{
			end_LBA = start_lsa_on_device + (end_LBA - end_lsa_on_device) - 1;
		}

		//Address access pattern statistics
		while (start_LBA <= end_LBA)
		{
			LPA_type device_address = Convert_host_logical_address_to_device_address(start_LBA);
			page_status_type access_status_bitmap = Find_NVM_subunit_access_bitmap(start_LBA);
			if (line_splitted[ASCIITraceTypeColumn].compare(ASCIITraceWriteCode) == 0)
			{
				if (stats.Write_address_access_pattern.find(device_address) == stats.Write_address_access_pattern.end())
				{
					Utils::Address_Histogram_Unit hist;
					hist.Access_count = 1;
					hist.Accessed_sub_units = access_status_bitmap;
					stats.Write_address_access_pattern[device_address] = hist;
				}
				else
				{
					stats.Write_address_access_pattern[device_address].Access_count = stats.Write_address_access_pattern[device_address].Access_count + 1;
					stats.Write_address_access_pattern[device_address].Accessed_sub_units = stats.Write_address_access_pattern[device_address].Accessed_sub_units | access_status_bitmap;
				}

				if (stats.Read_address_access_pattern.find(device_address) != stats.Read_address_access_pattern.end())
				{
					stats.Write_read_shared_addresses.insert(device_address);
				}
			}
			else
			{
				if (stats.Read_address_access_pattern.find(device_address) == stats.Read_address_access_pattern.end())
				{
					Utils::Address_Histogram_Unit hist;
					hist.Access_count = 1;
					hist.Accessed_sub_units = access_status_bitmap;
					stats.Read_address_access_pattern[device_address] = hist;
				}
				else
				{
					stats.Read_address_access_pattern[device_address].Access_count = stats.Read_address_access_pattern[device_address].Access_count + 1;
					stats.Read_address_access_pattern[device_address].Accessed_sub_units = stats.Read_address_access_pattern[device_address].Accessed_sub_units | access_status_bitmap;
				}

				if (stats.Write_address_access_pattern.find(device_address) != stats.Write_address_access_pattern.end())
				{
					stats.Write_read_shared_addresses.insert(device_address);
				}
			}
			stats.Total_accessed_lbas++;
			start_LBA++;
			if (start_LBA > end_lsa_on_device)
			{
				start_LBA = start_lsa_on_device;
			}
		}

		//Request size statistics
		if (line_splitted[ASCIITraceTypeColumn].compare(ASCIITraceWriteCode) == 0)
		{
			if (diff < MAX_ARRIVAL_TIME_HISTOGRAM)
			{
				stats.Write_arrival_time[diff]++;
			}
			else
			{
				stats.Write_arrival_time[MAX_ARRIVAL_TIME_HISTOGRAM]++;
			}

			if (LBA_count < MAX_REQSIZE_HISTOGRAM_ITEMS)
			{
				stats.Write_size_histogram[LBA_count]++;
			}
			else
			{
				stats.Write_size_histogram[MAX_REQSIZE_HISTOGRAM_ITEMS]++;
			}
		}
		else
		{
			if (diff < MAX_ARRIVAL_TIME_HISTOGRAM)
			{
				stats.Read_arrival_time[diff]++;
			}
			else
			{
				stats.Read_arrival_time[MAX_ARRIVAL_TIME_HISTOGRAM]++;
			}

			if (LBA_count < MAX_REQSIZE_HISTOGRAM_ITEMS)
			{
				stats.Read_size_histogram[LBA_count]++;
			}
			else
			{
				stats.Read_size_histogram[(unsigned int)MAX_REQSIZE_HISTOGRAM_ITEMS]++;
			}
		}
		stats.Total_generated_requests++;
	}
	trace_file_temp.close();
	if (stats.Total_generated_requests > 0)
	{
		stats.Average_request_size_sector = (unsigned int)(sum_request_size / stats.Total_generated_requests);
		stats.Average_inter_arrival_time_nano_sec = sum_inter_arrival / stats.Total_generated_requests;
	}
	else
	{
		stats.Average_request_size_sector = 0;
		stats.Average_inter_arrival_time_nano_sec = 0;
	}

	stats.Initial_occupancy_ratio = initial_occupancy_ratio;
	stats.Replay_no = total_replay_no;
}
} // namespace Host_Components
