#ifndef DEVICE_PARAMETER_SET_H
#define DEVICE_PARAMETER_SET_H

#include <string>
#include <vector>
#include "../ssd/SSD_Defs.h"
#include "../ssd/Host_Interface_Defs.h"
#include "../ssd/Host_Interface_Base.h"
#include "../ssd/Data_Cache_Manager_Base.h"
#include "../ssd/Address_Mapping_Unit_Base.h"
#include "../ssd/TSU_Base.h"
#include "../ssd/ONFI_Channel_Base.h"
#include "../ssd/GC_and_WL_Unit_Page_Level.h"
#include "../nvm_chip/NVM_Types.h"
#include "Parameter_Set_Base.h"
#include "Flash_Parameter_Set.h"

struct Flash_Media_Profile
{
	std::string Media_Profile_ID;
	Flash_Technology_Type Flash_Technology;
	NVM::FlashMemory::Command_Suspension_Mode CMD_Suspension_Support;
	sim_time_type Page_Read_Latency_LSB;
	sim_time_type Page_Read_Latency_CSB;
	sim_time_type Page_Read_Latency_MSB;
	sim_time_type Page_Program_Latency_LSB;
	sim_time_type Page_Program_Latency_CSB;
	sim_time_type Page_Program_Latency_MSB;
	sim_time_type Block_Erase_Latency;
	unsigned int Block_PE_Cycles_Limit;
	sim_time_type Suspend_Erase_Time;
	sim_time_type Suspend_Program_Time;

	Flash_Media_Profile();
	void XML_serialize(Utils::XmlWriter& xmlwriter) const;
	void XML_deserialize(rapidxml::xml_node<>* node);
};

struct Flash_Pool_Parameter_Set
{
	std::string Pool_ID;
	std::vector<flash_channel_ID_type> Channel_IDs;
	LHA_type Logical_Capacity_In_Sectors;
	std::string Media_Profile_ID;

	Flash_Pool_Parameter_Set();
	void XML_serialize(Utils::XmlWriter& xmlwriter) const;
	void XML_deserialize(rapidxml::xml_node<>* node);
};

class Device_Parameter_Set : public Parameter_Set_Base
{
public:
	Device_Parameter_Set();
	int Seed;//Seed for random number generation (used in device's random number generators)
	bool Enabled_Preconditioning;
	NVM::NVM_Type Memory_Type;
	HostInterface_Types HostInterface_Type;
	uint16_t IO_Queue_Depth;//For NVMe, it determines the size of the submission/completion queues; for SATA, it determines the size of NCQ_Control_Structure
	uint16_t Queue_Fetch_Size;//Used in NVMe host interface
	SSD_Components::Caching_Mechanism Caching_Mechanism;
	SSD_Components::Cache_Sharing_Mode Data_Cache_Sharing_Mode;//Data cache sharing among concurrently running I/O flows, if NVMe host interface is used
	unsigned int Data_Cache_Capacity;//Data cache capacity in bytes
	unsigned int Data_Cache_DRAM_Row_Size;//The row size of DRAM in the data cache, the unit is bytes
	unsigned int Data_Cache_DRAM_Data_Rate;//Data access rate to access DRAM in the data cache, the unit is MT/s
	unsigned int Data_Cache_DRAM_Data_Busrt_Size;//The number of bytes that are transferred in one burst (it depends on the number of DRAM chips)
	sim_time_type Data_Cache_DRAM_tRCD;//tRCD parameter to access DRAM in the data cache, the unit is nano-seconds
	sim_time_type Data_Cache_DRAM_tCL;//tCL parameter to access DRAM in the data cache, the unit is nano-seconds
	sim_time_type Data_Cache_DRAM_tRP;//tRP parameter to access DRAM in the data cache, the unit is nano-seconds
	SSD_Components::Flash_Address_Mapping_Type Address_Mapping;
	bool Ideal_Mapping_Table;//If mapping is ideal, then all the mapping entries are found in the DRAM and there is no need to read mapping entries from flash
	unsigned int CMT_Capacity;//Size of SRAM/DRAM space that is used to cache address mapping table, the unit is bytes
	SSD_Components::CMT_Sharing_Mode CMT_Sharing_Mode;//How the entire CMT space is shared among concurrently running flows
	SSD_Components::Flash_Plane_Allocation_Scheme_Type Plane_Allocation_Scheme;
	SSD_Components::Flash_Scheduling_Type Transaction_Scheduling_Policy;
	double Overprovisioning_Ratio;//The ratio of spare space with respect to the whole available storage space of SSD
	double GC_Exec_Threshold;//The threshold for the ratio of free pages that used to trigger GC
	SSD_Components::GC_Block_Selection_Policy_Type GC_Block_Selection_Policy;
	bool Use_Copyback_for_GC;
	bool Preemptible_GC_Enabled;
	double GC_Hard_Threshold;//The hard gc execution threshold, used to stop preemptible gc execution
	bool Dynamic_Wearleveling_Enabled;
	bool Static_Wearleveling_Enabled;
	unsigned int Static_Wearleveling_Threshold;
	sim_time_type Preferred_suspend_erase_time_for_read;//in nano-seconds, if the remaining time of the ongoing erase is smaller than Prefered_suspend_erase_time_for_read, then the ongoing erase operation will be suspended
	sim_time_type Preferred_suspend_erase_time_for_write;//in nano-seconds, if the remaining time of the ongoing erase is smaller than Prefered_suspend_erase_time_for_write, then the ongoing erase operation will be suspended
	sim_time_type Preferred_suspend_write_time_for_read;//in nano-seconds, if the remaining time of the ongoing write is smaller than Prefered_suspend_write_time_for_read, then the ongoing erase operation will be suspended
	unsigned int Flash_Channel_Count;
	unsigned int Flash_Channel_Width;//Channel width in byte
	unsigned int Channel_Transfer_Rate;//MT/s
	unsigned int Chip_No_Per_Channel;
	SSD_Components::ONFI_Protocol Flash_Comm_Protocol;
	Flash_Parameter_Set Flash_Parameters;
	std::vector<Flash_Media_Profile> Flash_Media_Profiles;
	std::vector<Flash_Pool_Parameter_Set> Flash_Pools;
	sim_time_type Measurement_Start_Time_Ns = 0;
	sim_time_type Measurement_End_Time_Ns = MAXIMUM_TIME;
	uint64_t Configuration_Hash = 0;
	const Flash_Media_Profile& Find_media_profile(const std::string& id) const;
	const Flash_Pool_Parameter_Set& Find_pool(const std::string& id) const;
	void XML_serialize(Utils::XmlWriter& xmlwriter);
	void XML_deserialize(rapidxml::xml_node<> *node);
};

#endif // !DEVICE_PARAMETER_SET_H
