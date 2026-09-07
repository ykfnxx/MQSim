#include <vector>
#include <stdexcept>
#include <ctime>
#include <algorithm>
#include <map>
#include <set>
#include "SSD_Device.h"
#include "../ssd/ONFI_Channel_Base.h"
#include "../ssd/Flash_Block_Manager.h"
#include "../ssd/Data_Cache_Manager_Flash_Advanced.h"
#include "../ssd/Data_Cache_Manager_Flash_Simple.h"
#include "../ssd/Address_Mapping_Unit_Base.h"
#include "../ssd/Address_Mapping_Unit_Page_Level.h"
#include "../ssd/Address_Mapping_Unit_Hybrid.h"
#include "../ssd/GC_and_WL_Unit_Page_Level.h"
#include "../ssd/TSU_OutofOrder.h"
#include "../ssd/TSU_Priority_OutOfOrder.h"
#include "../ssd/TSU_FLIN.h"
#include "../ssd/ONFI_Channel_NVDDR2.h"
#include "../ssd/NVM_PHY_ONFI_NVDDR2.h"
#include "../ssd/Stats.h"
#include "../utils/Logical_Address_Partitioning_Unit.h"

namespace
{
	std::string channel_list(const std::vector<flash_channel_ID_type>& channels)
	{
		std::string result;
		for (size_t index = 0; index < channels.size(); ++index) {
			if (index != 0) result += ',';
			result += std::to_string(channels[index]);
		}
		return result;
	}
}

SSD_Device *SSD_Device::my_instance; //Used in static functions

SSD_Device::SSD_Device(Device_Parameter_Set *parameters, std::vector<IO_Flow_Parameter_Set *> *io_flows)
	: MQSimEngine::Sim_Object("SSDDevice"), parameters(parameters), io_flows(io_flows)
{
	SSD_Device *device = this;
	my_instance = device; //used for static functions
	Simulator->AddObject(device);

	device->Preconditioning_required = parameters->Enabled_Preconditioning;
	device->Memory_Type = parameters->Memory_Type;

	switch (Memory_Type)
	{
	case NVM::NVM_Type::FLASH:
	{
		std::vector<sim_time_type> average_flash_read_latency, average_flash_write_latency;
		if (parameters->Flash_Media_Profiles.empty() || parameters->Flash_Pools.empty()) {
			PRINT_ERROR("Flash_Media_Profile and Flash_Pool_Parameter_Set are required")
		}
		if (parameters->Use_Copyback_for_GC) PRINT_ERROR("Copyback GC is not supported with TRIM-aware relocation")
		std::vector<const Flash_Media_Profile*> channel_profiles(parameters->Flash_Channel_Count, NULL);
		std::vector<unsigned int> channel_pe_cycle_limits(parameters->Flash_Channel_Count, 0);
		std::set<std::string> pool_ids;
		std::set<std::string> profile_ids;
		for (const auto& profile : parameters->Flash_Media_Profiles) {
			if (profile.Media_Profile_ID.empty() || !profile_ids.insert(profile.Media_Profile_ID).second) {
				PRINT_ERROR("Flash media profile IDs must be non-empty and unique")
			}
			if (profile.Block_PE_Cycles_Limit == 0) PRINT_ERROR("Flash media profile PE cycle limit must be positive")
		}
		unsigned int maximum_pe_cycles = 0;
		for (const auto& pool : parameters->Flash_Pools) {
			if (pool.Pool_ID.empty() || !pool_ids.insert(pool.Pool_ID).second || pool.Channel_IDs.empty()) {
				PRINT_ERROR("Flash pool IDs must be unique and each pool must contain channels")
			}
			const Flash_Media_Profile& profile = parameters->Find_media_profile(pool.Media_Profile_ID);
			maximum_pe_cycles = std::max(maximum_pe_cycles, profile.Block_PE_Cycles_Limit);
			for (auto channel : pool.Channel_IDs) {
				if (channel >= parameters->Flash_Channel_Count || channel_profiles[channel] != NULL) {
					PRINT_ERROR("Flash pool channels must be valid and disjoint")
				}
				channel_profiles[channel] = &profile;
				channel_pe_cycle_limits[channel] = profile.Block_PE_Cycles_Limit;
			}
			const LHA_type physical = static_cast<LHA_type>(pool.Channel_IDs.size()) * parameters->Chip_No_Per_Channel *
				parameters->Flash_Parameters.Die_No_Per_Chip * parameters->Flash_Parameters.Plane_No_Per_Die *
				parameters->Flash_Parameters.Block_No_Per_Plane * parameters->Flash_Parameters.Page_No_Per_Block *
				(parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE);
			if (pool.Logical_Capacity_In_Sectors == 0 ||
				pool.Logical_Capacity_In_Sectors > static_cast<LHA_type>(physical * (1.0 - parameters->Overprovisioning_Ratio))) {
				PRINT_ERROR("Invalid logical capacity for flash pool " << pool.Pool_ID)
			}
		}
		for (unsigned int channel = 0; channel < channel_profiles.size(); ++channel) {
			if (channel_profiles[channel] == NULL) PRINT_ERROR("Channel " << channel << " is not assigned to a flash pool")
		}
		for (const auto* flow : *io_flows) {
			const Flash_Media_Profile& profile = parameters->Find_media_profile(parameters->Find_pool(flow->Pool_ID).Media_Profile_ID);
			const unsigned int levels = static_cast<unsigned int>(profile.Flash_Technology);
			const sim_time_type read_sum = profile.Page_Read_Latency_LSB +
				(levels > 1 ? profile.Page_Read_Latency_MSB : 0) + (levels > 2 ? profile.Page_Read_Latency_CSB : 0);
			const sim_time_type write_sum = profile.Page_Program_Latency_LSB +
				(levels > 1 ? profile.Page_Program_Latency_MSB : 0) + (levels > 2 ? profile.Page_Program_Latency_CSB : 0);
			average_flash_read_latency.push_back(read_sum / levels);
			average_flash_write_latency.push_back(write_sum / levels);
		}

		//Step 2: create memory channels to connect chips to the controller
		this->Channel_count = parameters->Flash_Channel_Count;
		this->Chip_no_per_channel = parameters->Chip_No_Per_Channel;
		switch (parameters->Flash_Comm_Protocol)
		{
		case SSD_Components::ONFI_Protocol::NVDDR2:
		{
			SSD_Components::ONFI_Channel_NVDDR2 **channels = new SSD_Components::ONFI_Channel_NVDDR2 *[parameters->Flash_Channel_Count];
				for (unsigned int channel_cntr = 0; channel_cntr < parameters->Flash_Channel_Count; channel_cntr++)
				{
					const Flash_Media_Profile& profile = *channel_profiles[channel_cntr];
					const unsigned int levels = static_cast<unsigned int>(profile.Flash_Technology);
					sim_time_type read_latencies[3] = {profile.Page_Read_Latency_LSB, profile.Page_Read_Latency_MSB, profile.Page_Read_Latency_CSB};
					sim_time_type write_latencies[3] = {profile.Page_Program_Latency_LSB, profile.Page_Program_Latency_MSB, profile.Page_Program_Latency_CSB};
					if (levels == 3) {
						read_latencies[1] = profile.Page_Read_Latency_CSB;
						read_latencies[2] = profile.Page_Read_Latency_MSB;
						write_latencies[1] = profile.Page_Program_Latency_CSB;
						write_latencies[2] = profile.Page_Program_Latency_MSB;
					}
					NVM::FlashMemory::Flash_Chip **chips = new NVM::FlashMemory::Flash_Chip *[parameters->Chip_No_Per_Channel];
				for (unsigned int chip_cntr = 0; chip_cntr < parameters->Chip_No_Per_Channel; chip_cntr++)
				{
					chips[chip_cntr] = new NVM::FlashMemory::Flash_Chip(device->ID() + ".Channel." + std::to_string(channel_cntr) + ".Chip." + std::to_string(chip_cntr),
															channel_cntr, chip_cntr, profile.Flash_Technology, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
														parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
												read_latencies, write_latencies, profile.Block_Erase_Latency,
												profile.Suspend_Program_Time, profile.Suspend_Erase_Time,
												parameters->Flash_Parameters.Page_Capacity,
												parameters->Measurement_Start_Time_Ns, parameters->Measurement_End_Time_Ns);
					Simulator->AddObject(chips[chip_cntr]); //Each simulation object (a child of MQSimEngine::Sim_Object) should be added to the engine
				}
				channels[channel_cntr] = new SSD_Components::ONFI_Channel_NVDDR2(channel_cntr, parameters->Chip_No_Per_Channel,
																				 chips, parameters->Flash_Channel_Width,
																				 (sim_time_type)((double)1000 / parameters->Channel_Transfer_Rate) * 2, (sim_time_type)((double)1000 / parameters->Channel_Transfer_Rate) * 2);
				device->Channels.push_back(channels[channel_cntr]); //Channels should not be added to the simulator core, they are passive object that do not handle any simulation event
			}

			//Step 3: create channel controller and connect channels to it
			device->PHY = new SSD_Components::NVM_PHY_ONFI_NVDDR2(device->ID() + ".PHY", channels, parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
																  parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die);
			Simulator->AddObject(device->PHY);
			break;
		}
		default:
			throw std::invalid_argument("No implementation is available for the specified flash communication protocol");
		}
		//Steps 4 - 8: create FTL components and connect them together
		SSD_Components::FTL *ftl = new SSD_Components::FTL(device->ID() + ".FTL", NULL, parameters->Flash_Channel_Count,
														   parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
														   parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
														   parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, average_flash_read_latency, average_flash_write_latency, parameters->Overprovisioning_Ratio,
														   maximum_pe_cycles, parameters->Seed++);
		ftl->PHY = (SSD_Components::NVM_PHY_ONFI *)PHY;
		Simulator->AddObject(ftl);
		device->Firmware = ftl;

		//Step 5: create TSU
		SSD_Components::TSU_Base *tsu;
		std::vector<bool> erase_suspension(parameters->Flash_Channel_Count, false);
		std::vector<bool> program_suspension(parameters->Flash_Channel_Count, false);
		for (unsigned int channel = 0; channel < parameters->Flash_Channel_Count; ++channel) {
			const auto mode = channel_profiles[channel]->CMD_Suspension_Support;
			program_suspension[channel] = mode == NVM::FlashMemory::Command_Suspension_Mode::PROGRAM ||
				mode == NVM::FlashMemory::Command_Suspension_Mode::PROGRAM_ERASE;
			erase_suspension[channel] = mode == NVM::FlashMemory::Command_Suspension_Mode::ERASE ||
				mode == NVM::FlashMemory::Command_Suspension_Mode::PROGRAM_ERASE;
		}
		switch (parameters->Transaction_Scheduling_Policy)
		{
		case SSD_Components::Flash_Scheduling_Type::OUT_OF_ORDER:
			tsu = new SSD_Components::TSU_OutOfOrder(ftl->ID() + ".TSU", ftl, static_cast<SSD_Components::NVM_PHY_ONFI_NVDDR2 *>(device->PHY),
													 parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
													 parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
													 parameters->Preferred_suspend_write_time_for_read, parameters->Preferred_suspend_erase_time_for_read,
													 parameters->Preferred_suspend_erase_time_for_write,
													 erase_suspension, program_suspension);
			break;
		case SSD_Components::Flash_Scheduling_Type::PRIORITY_OUT_OF_ORDER:
			tsu = new SSD_Components::TSU_Priority_OutOfOrder(ftl->ID() + ".TSU", ftl, static_cast<SSD_Components::NVM_PHY_ONFI_NVDDR2 *>(device->PHY),
										  parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
										  parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
										  parameters->Preferred_suspend_write_time_for_read, parameters->Preferred_suspend_erase_time_for_read,
										  parameters->Preferred_suspend_erase_time_for_write,
										  erase_suspension, program_suspension);
			break;
		/*case SSD_Components::Flash_Scheduling_Type::FLIN:
				{
					unsigned int * stream_count_per_priority_class = new unsigned int[4];
					for (int i = 0; i < 4; i++)
						stream_count_per_priority_class[i] = 0;
					for (auto &flow : (*io_flows))
						stream_count_per_priority_class[(int)flow->Priority_Class]++;
					stream_id_type** stream_ids_per_priority_class = new stream_id_type*[4];
					for (int i = 0; i < 4; i++)
						stream_ids_per_priority_class[i] = new stream_id_type[stream_count_per_priority_class[i]];

					tsu = new SSD_Components::TSU_FLIN(ftl->ID() + ".TSU", ftl, static_cast<SSD_Components::NVM_PHY_ONFI_NVDDR2*>(device->PHY),
						parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
						parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die, parameters->Flash_Parameters.Page_Capacity,
						10000000, 33554432, 262144, 4, (unsigned int)io_flows->size(), stream_count_per_priority_class, stream_ids_per_priority_class,
						0.6,
						parameters->Preferred_suspend_write_time_for_read, parameters->Preferred_suspend_erase_time_for_read, parameters->Preferred_suspend_erase_time_for_write,
						erase_suspension, program_suspension);
					break;
				}*/
		default:
			throw std::invalid_argument("No implementation is available for the specified transaction scheduling algorithm");
		}
		Simulator->AddObject(tsu);
		ftl->TSU = tsu;

		//Step 6: create Flash_Block_Manager
		SSD_Components::Flash_Block_Manager_Base *fbm;
			fbm = new SSD_Components::Flash_Block_Manager(NULL, maximum_pe_cycles,
															  (unsigned int)io_flows->size(), channel_pe_cycle_limits, parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
															  parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
															  parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
															  parameters->Measurement_Start_Time_Ns, parameters->Measurement_End_Time_Ns);
		ftl->BlockManager = fbm;

		//Step 7: create Address_Mapping_Unit
		SSD_Components::Address_Mapping_Unit_Base *amu;
		std::vector<std::vector<flash_channel_ID_type>> flow_channel_id_assignments;
		std::vector<std::vector<flash_chip_ID_type>> flow_chip_id_assignments;
		std::vector<std::vector<flash_die_ID_type>> flow_die_id_assignments;
		std::vector<std::vector<flash_plane_ID_type>> flow_plane_id_assignments;
		std::vector<LHA_type> flow_logical_capacities;
		unsigned int stream_count = 0;
		for (unsigned int i = 0; i < io_flows->size(); i++)
		{
			const Flash_Pool_Parameter_Set& pool = parameters->Find_pool((*io_flows)[i]->Pool_ID);
			flow_channel_id_assignments.push_back(pool.Channel_IDs);
			flow_chip_id_assignments.push_back(std::vector<flash_chip_ID_type>());
			for (unsigned int id = 0; id < parameters->Chip_No_Per_Channel; ++id) flow_chip_id_assignments.back().push_back(id);
			flow_die_id_assignments.push_back(std::vector<flash_die_ID_type>());
			for (unsigned int id = 0; id < parameters->Flash_Parameters.Die_No_Per_Chip; ++id) flow_die_id_assignments.back().push_back(id);
			flow_plane_id_assignments.push_back(std::vector<flash_plane_ID_type>());
			for (unsigned int id = 0; id < parameters->Flash_Parameters.Plane_No_Per_Die; ++id) flow_plane_id_assignments.back().push_back(id);
			flow_logical_capacities.push_back(pool.Logical_Capacity_In_Sectors);
		}
		stream_count = parameters->HostInterface_Type == HostInterface_Types::SATA ? 1 : static_cast<unsigned int>(io_flows->size());

		Utils::Logical_Address_Partitioning_Unit::Allocate_logical_address_for_flows(parameters->HostInterface_Type, (unsigned int)io_flows->size(),
																		 parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
																		 flow_channel_id_assignments, flow_chip_id_assignments, flow_die_id_assignments, flow_plane_id_assignments,
																		 flow_logical_capacities,
																		 parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																					 parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Overprovisioning_Ratio);
		switch (parameters->Address_Mapping)
		{
		case SSD_Components::Flash_Address_Mapping_Type::PAGE_LEVEL:
			amu = new SSD_Components::Address_Mapping_Unit_Page_Level(ftl->ID() + ".AddressMappingUnit", ftl, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
																	  fbm, parameters->Ideal_Mapping_Table, parameters->CMT_Capacity, parameters->Plane_Allocation_Scheme, stream_count,
																	  parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
																	  flow_channel_id_assignments, flow_chip_id_assignments, flow_die_id_assignments, flow_plane_id_assignments,
																	  parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																	  parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Flash_Parameters.Page_Capacity, parameters->Overprovisioning_Ratio,
																	  parameters->CMT_Sharing_Mode);
			break;
		case SSD_Components::Flash_Address_Mapping_Type::HYBRID:
			amu = new SSD_Components::Address_Mapping_Unit_Hybrid(ftl->ID() + ".AddressMappingUnit", ftl, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
																  fbm, parameters->Ideal_Mapping_Table, stream_count,
																  parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip,
																  parameters->Flash_Parameters.Plane_No_Per_Die, parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																  parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Flash_Parameters.Page_Capacity, parameters->Overprovisioning_Ratio);
			break;
		default:
			throw std::invalid_argument("No implementation is available fo the secified address mapping strategy");
		}
		Simulator->AddObject(amu);
		ftl->Address_Mapping_Unit = amu;

		//Step 8: create GC_and_WL_unit
		double max_rho = 0;
		for (unsigned int i = 0; i < io_flows->size(); i++)
		{
			if ((*io_flows)[i]->Initial_Occupancy_Percentage > max_rho)
			{
				max_rho = (*io_flows)[i]->Initial_Occupancy_Percentage;
			}
		}
		max_rho /= 100; //Convert from percentage to a value between zero and 1
		SSD_Components::GC_and_WL_Unit_Base *gcwl;
		gcwl = new SSD_Components::GC_and_WL_Unit_Page_Level(ftl->ID() + ".GCandWLUnit", amu, fbm, tsu, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
															 parameters->GC_Block_Selection_Policy, parameters->GC_Exec_Threshold, parameters->Preemptible_GC_Enabled, parameters->GC_Hard_Threshold,
															 parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
															 parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
															 parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																 parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Use_Copyback_for_GC, max_rho, 1,
															 parameters->Dynamic_Wearleveling_Enabled, parameters->Static_Wearleveling_Enabled,
															 parameters->Static_Wearleveling_Threshold, parameters->Seed++);
		Simulator->AddObject(gcwl);
		fbm->Set_GC_and_WL_Unit(gcwl);
		ftl->GC_and_WL_Unit = gcwl;

		//Step 9: create Data_Cache_Manager
		SSD_Components::Data_Cache_Manager_Base *dcm;
		SSD_Components::Caching_Mode *caching_modes = new SSD_Components::Caching_Mode[io_flows->size()];
		for (unsigned int i = 0; i < io_flows->size(); i++)
		{
			caching_modes[i] = (*io_flows)[i]->Device_Level_Data_Caching_Mode;
		}

		switch (parameters->Caching_Mechanism)
		{
		case SSD_Components::Caching_Mechanism::SIMPLE:
			dcm = new SSD_Components::Data_Cache_Manager_Flash_Simple(device->ID() + ".DataCache", NULL, ftl, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
																	  parameters->Data_Cache_Capacity, parameters->Data_Cache_DRAM_Row_Size, parameters->Data_Cache_DRAM_Data_Rate,
																	  parameters->Data_Cache_DRAM_Data_Busrt_Size, parameters->Data_Cache_DRAM_tRCD, parameters->Data_Cache_DRAM_tCL, parameters->Data_Cache_DRAM_tRP,
																	  caching_modes, (unsigned int)io_flows->size(),
																	  parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Flash_Channel_Count * parameters->Chip_No_Per_Channel * parameters->Flash_Parameters.Die_No_Per_Chip * parameters->Flash_Parameters.Plane_No_Per_Die * parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE);

			break;
		case SSD_Components::Caching_Mechanism::ADVANCED:
			dcm = new SSD_Components::Data_Cache_Manager_Flash_Advanced(device->ID() + ".DataCache", NULL, ftl, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
																		parameters->Data_Cache_Capacity, parameters->Data_Cache_DRAM_Row_Size, parameters->Data_Cache_DRAM_Data_Rate,
																		parameters->Data_Cache_DRAM_Data_Busrt_Size, parameters->Data_Cache_DRAM_tRCD, parameters->Data_Cache_DRAM_tCL, parameters->Data_Cache_DRAM_tRP,
																		caching_modes, parameters->Data_Cache_Sharing_Mode, (unsigned int)io_flows->size(),
																		parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, parameters->Flash_Channel_Count * parameters->Chip_No_Per_Channel * parameters->Flash_Parameters.Die_No_Per_Chip * parameters->Flash_Parameters.Plane_No_Per_Die * parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE);

			break;
		default:
			PRINT_ERROR("Unknown data caching mechanism!")
		}

		Simulator->AddObject(dcm);
		ftl->Data_cache_manager = dcm;
		device->Cache_manager = dcm;

		//Step 10: create Host_Interface
		switch (parameters->HostInterface_Type)
		{
		case HostInterface_Types::NVME:
			device->Host_interface = new SSD_Components::Host_Interface_NVMe(device->ID() + ".HostInterface",
																			 Utils::Logical_Address_Partitioning_Unit::Get_total_device_lha_count(), parameters->IO_Queue_Depth, parameters->IO_Queue_Depth,
																			 (unsigned int)io_flows->size(), parameters->Queue_Fetch_Size, parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, dcm);
			break;
		case HostInterface_Types::SATA:
			device->Host_interface = new SSD_Components::Host_Interface_SATA(device->ID() + ".HostInterface",
																			 parameters->IO_Queue_Depth, Utils::Logical_Address_Partitioning_Unit::Get_total_device_lha_count(), parameters->Flash_Parameters.Page_Capacity / SECTOR_SIZE_IN_BYTE, dcm);

			break;
		default:
			break;
		}
		Simulator->AddObject(device->Host_interface);
		dcm->Set_host_interface(device->Host_interface);
		break;
	}
	default:
		throw std::invalid_argument("Undefined NVM type specified ");
	} // switch (Memory_Type)
}

SSD_Device::~SSD_Device()
{
	for (unsigned int channel_cntr = 0; channel_cntr < Channel_count; channel_cntr++)
	{
		for (unsigned int chip_cntr = 0; chip_cntr < Chip_no_per_channel; chip_cntr++)
		{
			delete ((SSD_Components::ONFI_Channel_NVDDR2 *)this->Channels[channel_cntr])->Chips[chip_cntr];
		}
		delete this->Channels[channel_cntr];
	}

	delete this->PHY;
	delete ((SSD_Components::FTL *)this->Firmware)->TSU;
	delete ((SSD_Components::FTL *)this->Firmware)->BlockManager;
	delete ((SSD_Components::FTL *)this->Firmware)->Address_Mapping_Unit;
	delete ((SSD_Components::FTL *)this->Firmware)->GC_and_WL_Unit;
	delete this->Firmware;
	delete this->Cache_manager;
	delete this->Host_interface;
}

void SSD_Device::Attach_to_host(Host_Components::PCIe_Switch *pcie_switch)
{
	this->Host_interface->Attach_to_device(pcie_switch);
}

void SSD_Device::Perform_preconditioning(std::vector<Utils::Workload_Statistics *> workload_stats)
{
	if (Preconditioning_required)
	{
		time_t start_time = time(0);
		PRINT_MESSAGE("SSD Device preconditioning started .........");
		this->Firmware->Perform_precondition(workload_stats);
		this->Cache_manager->Do_warmup(workload_stats);
		time_t end_time = time(0);
		uint64_t duration = (uint64_t)difftime(end_time, start_time);
		PRINT_MESSAGE("Finished preconditioning. Duration of preconditioning: " << duration / 3600 << ":" << (duration % 3600) / 60 << ":" << ((duration % 3600) % 60));
	}
}

void SSD_Device::Start_simulation()
{
}

void SSD_Device::Validate_simulation_config()
{
}

void SSD_Device::Validate_simulation_drained()
{
	SSD_Components::FTL* ftl = static_cast<SSD_Components::FTL*>(Firmware);
	if (!static_cast<SSD_Components::NVM_PHY_ONFI_NVDDR2*>(PHY)->Is_drained()) PRINT_ERROR("Simulation ended with pending NAND commands or transfers")
	if (!ftl->TSU->Is_drained()) PRINT_ERROR("Simulation ended with pending TSU transactions")
	if (!ftl->BlockManager->Is_drained()) PRINT_ERROR("Simulation ended with pending block-manager or GC work")
	if (!ftl->Address_Mapping_Unit->Is_drained()) PRINT_ERROR("Simulation ended with pending address-mapping work")
	if (!Cache_manager->Is_drained()) PRINT_ERROR("Simulation ended with pending data-cache work")
	if (!Host_interface->Is_drained()) PRINT_ERROR("Simulation ended with pending host-interface queue entries")
}

void SSD_Device::Execute_simulator_event(MQSimEngine::Sim_Event *event)
{
}

void SSD_Device::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter &xmlwriter)
{
	std::string tmp;
	tmp = ID();
	xmlwriter.Write_open_tag(tmp);

	this->Host_interface->Report_results_in_XML(ID(), xmlwriter);
	if (Memory_Type == NVM::NVM_Type::FLASH)
	{
		xmlwriter.Write_start_element_tag(ID() + ".Configuration");
		xmlwriter.Write_attribute_string_inline("Simulator_Version", "MQSim-DWPDSim-vNext-1");
		xmlwriter.Write_attribute_string_inline("Statistics_ABI_Version", "1");
		xmlwriter.Write_attribute_string_inline("Configuration_Hash", std::to_string(parameters->Configuration_Hash));
		xmlwriter.Write_attribute_string_inline("Configuration_Hash_Algorithm", "fnv1a64-raw-xml");
		xmlwriter.Write_attribute_string_inline("Time_Unit", "nanosecond");
		xmlwriter.Write_attribute_string_inline("Measurement_Start_Time_Ns", std::to_string(parameters->Measurement_Start_Time_Ns));
		xmlwriter.Write_attribute_string_inline("Measurement_End_Time_Ns", std::to_string(parameters->Measurement_End_Time_Ns));
		xmlwriter.Write_end_element_tag();

		((SSD_Components::FTL *)this->Firmware)->Report_results_in_XML(ID(), xmlwriter);
		((SSD_Components::FTL *)this->Firmware)->TSU->Report_results_in_XML(ID(), xmlwriter);
		SSD_Components::Flash_Block_Manager_Base* block_manager = ((SSD_Components::FTL*)this->Firmware)->BlockManager;
		for (const auto& pool : parameters->Flash_Pools) {
			const Flash_Media_Profile& profile = parameters->Find_media_profile(pool.Media_Profile_ID);
			unsigned long long host_read_bytes = 0, host_write_bytes = 0, requested_trim_bytes = 0;
			unsigned long long measurement_host_write_bytes = 0, received_trim_count = 0;
			unsigned long long effective_trimmed_sectors = 0, pages_invalidated_by_trim = 0;
			unsigned long long gc_executions = 0, gc_page_reads = 0, gc_page_programs = 0;
			if (host_flows != NULL) {
				for (size_t stream = 0; stream < host_flows->size(); ++stream) {
					if ((*io_flows)[stream]->Pool_ID != pool.Pool_ID) continue;
					host_read_bytes += (*host_flows)[stream]->Get_read_bytes();
					host_write_bytes += (*host_flows)[stream]->Get_write_bytes();
					requested_trim_bytes += (*host_flows)[stream]->Get_trim_bytes();
					measurement_host_write_bytes += (*host_flows)[stream]->Get_measurement_write_bytes();
					received_trim_count += SSD_Components::Stats::Total_received_trim_commands_per_stream[stream];
					effective_trimmed_sectors += SSD_Components::Stats::Total_trimmed_sectors_per_stream[stream];
					pages_invalidated_by_trim += SSD_Components::Stats::Total_pages_invalidated_by_trim_per_stream[stream];
					gc_executions += SSD_Components::Stats::Total_gc_executions_per_stream[stream];
					gc_page_reads += SSD_Components::Stats::Total_gc_page_reads_per_stream[stream];
					gc_page_programs += SSD_Components::Stats::Total_gc_page_programs_per_stream[stream];
				}
			}
			unsigned long long flash_reads = 0, flash_programs = 0, flash_erases = 0;
			unsigned long long measurement_programs = 0, total_erases = 0, measurement_erases = 0;
			unsigned int max_erases = 0, measurement_max_erases = 0;
			for (flash_channel_ID_type channel : pool.Channel_IDs) {
				for (unsigned int chip = 0; chip < Chip_no_per_channel; ++chip) {
					NVM::FlashMemory::Flash_Chip* flash_chip = ((SSD_Components::ONFI_Channel_NVDDR2*)Channels[channel])->Chips[chip];
					flash_reads += flash_chip->Get_read_count();
					flash_programs += flash_chip->Get_program_count();
					flash_erases += flash_chip->Get_erase_count();
					measurement_programs += flash_chip->Get_measurement_program_count();
				}
				total_erases += block_manager->Get_total_erase_count_for_channel(channel);
				max_erases = std::max(max_erases, block_manager->Get_max_erase_count_for_channel(channel));
				measurement_erases += block_manager->Get_total_measurement_erase_count_for_channel(channel);
				measurement_max_erases = std::max(measurement_max_erases, block_manager->Get_max_measurement_erase_count_for_channel(channel));
			}
			const unsigned long long physical_capacity_bytes = static_cast<unsigned long long>(pool.Channel_IDs.size()) *
				parameters->Chip_No_Per_Channel * parameters->Flash_Parameters.Die_No_Per_Chip * parameters->Flash_Parameters.Plane_No_Per_Die *
				parameters->Flash_Parameters.Block_No_Per_Plane * parameters->Flash_Parameters.Page_No_Per_Block * parameters->Flash_Parameters.Page_Capacity;
			xmlwriter.Write_start_element_tag(ID() + ".Pool");
			xmlwriter.Write_attribute_string_inline("ID", pool.Pool_ID);
			xmlwriter.Write_attribute_string_inline("Media_Profile_ID", pool.Media_Profile_ID);
			xmlwriter.Write_attribute_string_inline("Channel_IDs", channel_list(pool.Channel_IDs));
			xmlwriter.Write_attribute_string_inline("Host_Read_Bytes", std::to_string(host_read_bytes));
			xmlwriter.Write_attribute_string_inline("Host_Write_Bytes", std::to_string(host_write_bytes));
			xmlwriter.Write_attribute_string_inline("Requested_Trim_Bytes", std::to_string(requested_trim_bytes));
			xmlwriter.Write_attribute_string_inline("Effective_Trimmed_Bytes", std::to_string(effective_trimmed_sectors * SECTOR_SIZE_IN_BYTE));
			xmlwriter.Write_attribute_string_inline("Received_Trim_Command_Count", std::to_string(received_trim_count));
			xmlwriter.Write_attribute_string_inline("Requested_Trim_Sector_Count", std::to_string(requested_trim_bytes / SECTOR_SIZE_IN_BYTE));
			xmlwriter.Write_attribute_string_inline("Effective_Trimmed_Sector_Count", std::to_string(effective_trimmed_sectors));
			xmlwriter.Write_attribute_string_inline("GC_Execution_Count", std::to_string(gc_executions));
			xmlwriter.Write_attribute_string_inline("GC_Page_Read_Count", std::to_string(gc_page_reads));
			xmlwriter.Write_attribute_string_inline("GC_Page_Program_Count", std::to_string(gc_page_programs));
			xmlwriter.Write_attribute_string_inline("Pages_Invalidated_By_Trim", std::to_string(pages_invalidated_by_trim));
			xmlwriter.Write_attribute_string_inline("Flash_Read_Command_Count", std::to_string(flash_reads));
			xmlwriter.Write_attribute_string_inline("Flash_Program_Command_Count", std::to_string(flash_programs));
			xmlwriter.Write_attribute_string_inline("Flash_Erase_Command_Count", std::to_string(flash_erases));
			xmlwriter.Write_attribute_string_inline("Total_Block_Erase_Count", std::to_string(total_erases));
			xmlwriter.Write_attribute_string_inline("Max_Block_Erase_Count", std::to_string(max_erases));
			xmlwriter.Write_attribute_string_inline("Measurement_Total_Block_Erase_Count", std::to_string(measurement_erases));
			xmlwriter.Write_attribute_string_inline("Measurement_Max_Block_Erase_Count", std::to_string(measurement_max_erases));
			xmlwriter.Write_attribute_string_inline("Measurement_Host_Write_Bytes", std::to_string(measurement_host_write_bytes));
			xmlwriter.Write_attribute_string_inline("Measurement_Flash_Programmed_Bytes", std::to_string(measurement_programs * parameters->Flash_Parameters.Page_Capacity));
			xmlwriter.Write_attribute_string_inline("Logical_Capacity_Bytes", std::to_string(pool.Logical_Capacity_In_Sectors * SECTOR_SIZE_IN_BYTE));
			xmlwriter.Write_attribute_string_inline("Physical_Capacity_Bytes", std::to_string(physical_capacity_bytes));
			xmlwriter.Write_attribute_string_inline("PE_Cycle_Limit", std::to_string(profile.Block_PE_Cycles_Limit));
			xmlwriter.Write_end_element_tag();
		}

		for (unsigned int channel_cntr = 0; channel_cntr < Channel_count; channel_cntr++)
		{
			const Flash_Pool_Parameter_Set* channel_pool = NULL;
			for (const auto& pool : parameters->Flash_Pools) {
				if (std::find(pool.Channel_IDs.begin(), pool.Channel_IDs.end(), channel_cntr) != pool.Channel_IDs.end()) channel_pool = &pool;
			}
			const Flash_Media_Profile& profile = parameters->Find_media_profile(channel_pool->Media_Profile_ID);
			unsigned long long flash_reads = 0, flash_programs = 0, flash_erases = 0, measurement_programs = 0;
			for (unsigned int chip_cntr = 0; chip_cntr < Chip_no_per_channel; chip_cntr++)
			{
				NVM::FlashMemory::Flash_Chip* chip = ((SSD_Components::ONFI_Channel_NVDDR2 *)Channels[channel_cntr])->Chips[chip_cntr];
				flash_reads += chip->Get_read_count();
				flash_programs += chip->Get_program_count();
				flash_erases += chip->Get_erase_count();
				measurement_programs += chip->Get_measurement_program_count();
				chip->Report_results_in_XML(ID(), xmlwriter);
			}
			const unsigned long long physical_capacity_bytes = static_cast<unsigned long long>(parameters->Chip_No_Per_Channel) *
				parameters->Flash_Parameters.Die_No_Per_Chip * parameters->Flash_Parameters.Plane_No_Per_Die * parameters->Flash_Parameters.Block_No_Per_Plane *
				parameters->Flash_Parameters.Page_No_Per_Block * parameters->Flash_Parameters.Page_Capacity;
			xmlwriter.Write_start_element_tag(ID() + ".Channel");
			xmlwriter.Write_attribute_string_inline("ID", std::to_string(channel_cntr));
			xmlwriter.Write_attribute_string_inline("Pool_ID", channel_pool->Pool_ID);
			xmlwriter.Write_attribute_string_inline("Media_Profile_ID", channel_pool->Media_Profile_ID);
			xmlwriter.Write_attribute_string_inline("Host_Read_Bytes", std::to_string(SSD_Components::Stats::Channel_host_read_bytes[channel_cntr]));
			xmlwriter.Write_attribute_string_inline("Host_Write_Bytes", std::to_string(SSD_Components::Stats::Channel_host_write_bytes[channel_cntr]));
			xmlwriter.Write_attribute_string_inline("Requested_Trim_Bytes", std::to_string(SSD_Components::Stats::Channel_requested_trim_sectors[channel_cntr] * SECTOR_SIZE_IN_BYTE));
			xmlwriter.Write_attribute_string_inline("Effective_Trimmed_Bytes", std::to_string(SSD_Components::Stats::Channel_effective_trimmed_sectors[channel_cntr] * SECTOR_SIZE_IN_BYTE));
			xmlwriter.Write_attribute_string_inline("Requested_Trim_Sector_Count", std::to_string(SSD_Components::Stats::Channel_requested_trim_sectors[channel_cntr]));
			xmlwriter.Write_attribute_string_inline("Effective_Trimmed_Sector_Count", std::to_string(SSD_Components::Stats::Channel_effective_trimmed_sectors[channel_cntr]));
			xmlwriter.Write_attribute_string_inline("Flash_Read_Command_Count", std::to_string(flash_reads));
			xmlwriter.Write_attribute_string_inline("Flash_Program_Command_Count", std::to_string(flash_programs));
			xmlwriter.Write_attribute_string_inline("Flash_Erase_Command_Count", std::to_string(flash_erases));
			xmlwriter.Write_attribute_string_inline("Total_Block_Erase_Count", std::to_string(block_manager->Get_total_erase_count_for_channel(channel_cntr)));
			xmlwriter.Write_attribute_string_inline("Max_Block_Erase_Count", std::to_string(block_manager->Get_max_erase_count_for_channel(channel_cntr)));
			xmlwriter.Write_attribute_string_inline("Measurement_Total_Block_Erase_Count", std::to_string(block_manager->Get_total_measurement_erase_count_for_channel(channel_cntr)));
			xmlwriter.Write_attribute_string_inline("Measurement_Max_Block_Erase_Count", std::to_string(block_manager->Get_max_measurement_erase_count_for_channel(channel_cntr)));
			xmlwriter.Write_attribute_string_inline("Measurement_Flash_Programmed_Bytes", std::to_string(measurement_programs * parameters->Flash_Parameters.Page_Capacity));
			xmlwriter.Write_attribute_string_inline("Logical_Capacity_Bytes", std::to_string(channel_pool->Logical_Capacity_In_Sectors * SECTOR_SIZE_IN_BYTE / channel_pool->Channel_IDs.size()));
			xmlwriter.Write_attribute_string_inline("Physical_Capacity_Bytes", std::to_string(physical_capacity_bytes));
			xmlwriter.Write_attribute_string_inline("PE_Cycle_Limit", std::to_string(profile.Block_PE_Cycles_Limit));
			xmlwriter.Write_end_element_tag();
		}
	}
	xmlwriter.Write_close_tag();
}

unsigned int SSD_Device::Get_no_of_LHAs_in_an_NVM_write_unit()
{
	return Host_interface->Get_no_of_LHAs_in_an_NVM_write_unit();
}

LPA_type SSD_Device::Convert_host_logical_address_to_device_address(LHA_type lha)
{
	return my_instance->Firmware->Convert_host_logical_address_to_device_address(lha);
}

page_status_type SSD_Device::Find_NVM_subunit_access_bitmap(LHA_type lha)
{
	return my_instance->Firmware->Find_NVM_subunit_access_bitmap(lha);
}
