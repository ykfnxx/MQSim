#include "Device_Parameter_Set.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <sstream>



Device_Parameter_Set::Device_Parameter_Set()
	: Seed(123), Enabled_Preconditioning(true), Memory_Type(NVM::NVM_Type::FLASH),
	  HostInterface_Type(HostInterface_Types::NVME), IO_Queue_Depth(1024), Queue_Fetch_Size(512),
	  Caching_Mechanism(SSD_Components::Caching_Mechanism::ADVANCED),
	  Data_Cache_Sharing_Mode(SSD_Components::Cache_Sharing_Mode::SHARED),
	  Data_Cache_Capacity(1024 * 1024 * 512), Data_Cache_DRAM_Row_Size(8192),
	  Data_Cache_DRAM_Data_Rate(800), Data_Cache_DRAM_Data_Busrt_Size(4),
	  Data_Cache_DRAM_tRCD(13), Data_Cache_DRAM_tCL(13), Data_Cache_DRAM_tRP(13),
	  Address_Mapping(SSD_Components::Flash_Address_Mapping_Type::PAGE_LEVEL), Ideal_Mapping_Table(false),
	  CMT_Capacity(2 * 1024 * 1024), CMT_Sharing_Mode(SSD_Components::CMT_Sharing_Mode::SHARED),
	  Plane_Allocation_Scheme(SSD_Components::Flash_Plane_Allocation_Scheme_Type::CWDP),
	  Transaction_Scheduling_Policy(SSD_Components::Flash_Scheduling_Type::OUT_OF_ORDER),
	  Overprovisioning_Ratio(0.07), GC_Exec_Threshold(0.05),
	  GC_Block_Selection_Policy(SSD_Components::GC_Block_Selection_Policy_Type::RGA),
	  Use_Copyback_for_GC(false), Preemptible_GC_Enabled(true), GC_Hard_Threshold(0.005),
	  Dynamic_Wearleveling_Enabled(true), Static_Wearleveling_Enabled(true), Static_Wearleveling_Threshold(100),
	  Preferred_suspend_erase_time_for_read(700000), Preferred_suspend_erase_time_for_write(700000),
	  Preferred_suspend_write_time_for_read(100000), Flash_Channel_Count(8), Flash_Channel_Width(1),
	  Channel_Transfer_Rate(300), Chip_No_Per_Channel(4),
	  Flash_Comm_Protocol(SSD_Components::ONFI_Protocol::NVDDR2)
{
}
Flash_Media_Profile::Flash_Media_Profile()
	: Flash_Technology(Flash_Technology_Type::SLC),
	  CMD_Suspension_Support(NVM::FlashMemory::Command_Suspension_Mode::NONE),
	  Page_Read_Latency_LSB(75000), Page_Read_Latency_CSB(75000), Page_Read_Latency_MSB(75000),
	  Page_Program_Latency_LSB(750000), Page_Program_Latency_CSB(750000), Page_Program_Latency_MSB(750000),
	  Block_Erase_Latency(3800000), Block_PE_Cycles_Limit(10000),
	  Suspend_Erase_Time(700000), Suspend_Program_Time(100000)
{
}

Flash_Pool_Parameter_Set::Flash_Pool_Parameter_Set() : Logical_Capacity_In_Sectors(0)
{
}

static Flash_Technology_Type parse_flash_technology(const std::string& text)
{
	std::string value(text);
	std::transform(value.begin(), value.end(), value.begin(), ::toupper);
	if (value == "SLC") return Flash_Technology_Type::SLC;
	if (value == "MLC") return Flash_Technology_Type::MLC;
	if (value == "TLC") return Flash_Technology_Type::TLC;
	PRINT_ERROR("Unknown flash technology: " << text)
}

static NVM::FlashMemory::Command_Suspension_Mode parse_suspension_mode(const std::string& text)
{
	std::string value(text);
	std::transform(value.begin(), value.end(), value.begin(), ::toupper);
	if (value == "NONE") return NVM::FlashMemory::Command_Suspension_Mode::NONE;
	if (value == "PROGRAM") return NVM::FlashMemory::Command_Suspension_Mode::PROGRAM;
	if (value == "ERASE") return NVM::FlashMemory::Command_Suspension_Mode::ERASE;
	if (value == "PROGRAM_ERASE") return NVM::FlashMemory::Command_Suspension_Mode::PROGRAM_ERASE;
	PRINT_ERROR("Unknown command suspension mode: " << text)
}

void Flash_Media_Profile::XML_deserialize(rapidxml::xml_node<>* node)
{
	for (auto param = node->first_node(); param; param = param->next_sibling()) {
		const std::string name(param->name());
		const std::string value(param->value());
		if (name == "Media_Profile_ID") Media_Profile_ID = value;
		else if (name == "Flash_Technology") Flash_Technology = parse_flash_technology(value);
		else if (name == "CMD_Suspension_Support") CMD_Suspension_Support = parse_suspension_mode(value);
		else if (name == "Page_Read_Latency_LSB") Page_Read_Latency_LSB = std::stoull(value);
		else if (name == "Page_Read_Latency_CSB") Page_Read_Latency_CSB = std::stoull(value);
		else if (name == "Page_Read_Latency_MSB") Page_Read_Latency_MSB = std::stoull(value);
		else if (name == "Page_Program_Latency_LSB") Page_Program_Latency_LSB = std::stoull(value);
		else if (name == "Page_Program_Latency_CSB") Page_Program_Latency_CSB = std::stoull(value);
		else if (name == "Page_Program_Latency_MSB") Page_Program_Latency_MSB = std::stoull(value);
		else if (name == "Block_Erase_Latency") Block_Erase_Latency = std::stoull(value);
		else if (name == "Block_PE_Cycles_Limit") Block_PE_Cycles_Limit = std::stoul(value);
		else if (name == "Suspend_Erase_Time") Suspend_Erase_Time = std::stoull(value);
		else if (name == "Suspend_Program_Time") Suspend_Program_Time = std::stoull(value);
	}
}

void Flash_Media_Profile::XML_serialize(Utils::XmlWriter& xmlwriter) const
{
	xmlwriter.Write_open_tag("Flash_Media_Profile");
	xmlwriter.Write_attribute_string("Media_Profile_ID", Media_Profile_ID);
	std::string technology = Flash_Technology == Flash_Technology_Type::SLC ? "SLC" :
		(Flash_Technology == Flash_Technology_Type::MLC ? "MLC" : "TLC");
	xmlwriter.Write_attribute_string("Flash_Technology", technology);
	std::string suspension = "NONE";
	if (CMD_Suspension_Support == NVM::FlashMemory::Command_Suspension_Mode::PROGRAM) suspension = "PROGRAM";
	else if (CMD_Suspension_Support == NVM::FlashMemory::Command_Suspension_Mode::ERASE) suspension = "ERASE";
	else if (CMD_Suspension_Support == NVM::FlashMemory::Command_Suspension_Mode::PROGRAM_ERASE) suspension = "PROGRAM_ERASE";
	xmlwriter.Write_attribute_string("CMD_Suspension_Support", suspension);
	xmlwriter.Write_attribute_string("Page_Read_Latency_LSB", std::to_string(Page_Read_Latency_LSB));
	xmlwriter.Write_attribute_string("Page_Read_Latency_CSB", std::to_string(Page_Read_Latency_CSB));
	xmlwriter.Write_attribute_string("Page_Read_Latency_MSB", std::to_string(Page_Read_Latency_MSB));
	xmlwriter.Write_attribute_string("Page_Program_Latency_LSB", std::to_string(Page_Program_Latency_LSB));
	xmlwriter.Write_attribute_string("Page_Program_Latency_CSB", std::to_string(Page_Program_Latency_CSB));
	xmlwriter.Write_attribute_string("Page_Program_Latency_MSB", std::to_string(Page_Program_Latency_MSB));
	xmlwriter.Write_attribute_string("Block_Erase_Latency", std::to_string(Block_Erase_Latency));
	xmlwriter.Write_attribute_string("Block_PE_Cycles_Limit", std::to_string(Block_PE_Cycles_Limit));
	xmlwriter.Write_attribute_string("Suspend_Erase_Time", std::to_string(Suspend_Erase_Time));
	xmlwriter.Write_attribute_string("Suspend_Program_Time", std::to_string(Suspend_Program_Time));
	xmlwriter.Write_close_tag();
}

void Flash_Pool_Parameter_Set::XML_deserialize(rapidxml::xml_node<>* node)
{
	for (auto param = node->first_node(); param; param = param->next_sibling()) {
		const std::string name(param->name());
		const std::string value(param->value());
		if (name == "Pool_ID") Pool_ID = value;
		else if (name == "Logical_Capacity_In_Sectors") Logical_Capacity_In_Sectors = std::stoull(value);
		else if (name == "Media_Profile_ID") Media_Profile_ID = value;
		else if (name == "Channel_IDs") {
			std::stringstream input(value);
			std::string item;
			while (std::getline(input, item, ',')) Channel_IDs.push_back(static_cast<flash_channel_ID_type>(std::stoul(item)));
		}
	}
}

void Flash_Pool_Parameter_Set::XML_serialize(Utils::XmlWriter& xmlwriter) const
{
	xmlwriter.Write_open_tag("Flash_Pool_Parameter_Set");
	xmlwriter.Write_attribute_string("Pool_ID", Pool_ID);
	std::string channels;
	for (size_t index = 0; index < Channel_IDs.size(); ++index) {
		if (index != 0) channels += ",";
		channels += std::to_string(Channel_IDs[index]);
	}
	xmlwriter.Write_attribute_string("Channel_IDs", channels);
	xmlwriter.Write_attribute_string("Logical_Capacity_In_Sectors", std::to_string(Logical_Capacity_In_Sectors));
	xmlwriter.Write_attribute_string("Media_Profile_ID", Media_Profile_ID);
	xmlwriter.Write_close_tag();
}

const Flash_Media_Profile& Device_Parameter_Set::Find_media_profile(const std::string& id) const
{
	for (const auto& profile : Flash_Media_Profiles) if (profile.Media_Profile_ID == id) return profile;
	PRINT_ERROR("Unknown flash media profile: " << id)
}

const Flash_Pool_Parameter_Set& Device_Parameter_Set::Find_pool(const std::string& id) const
{
	for (const auto& pool : Flash_Pools) if (pool.Pool_ID == id) return pool;
	PRINT_ERROR("Unknown flash pool: " << id)
}

void Device_Parameter_Set::XML_serialize(Utils::XmlWriter& xmlwriter)
{
	std::string tmp;
	tmp = "Device_Parameter_Set";
	xmlwriter.Write_open_tag(tmp);

	std::string attr = "Seed";
	std::string val = std::to_string(Seed);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Enabled_Preconditioning";
	val = (Enabled_Preconditioning ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Memory_Type";
	val;
	switch (Memory_Type) {
		case NVM::NVM_Type::FLASH:
			val = "FLASH";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "HostInterface_Type";
	val;
	switch (HostInterface_Type) {
		case HostInterface_Types::NVME:
			val = "NVME";
			break;
		case HostInterface_Types::SATA:
			val = "SATA";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "IO_Queue_Depth";
	val = std::to_string(IO_Queue_Depth);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Queue_Fetch_Size";
	val = std::to_string(Queue_Fetch_Size);
	xmlwriter.Write_attribute_string(attr, val);


	attr = "Caching_Mechanism";
	switch (Caching_Mechanism) {
		case SSD_Components::Caching_Mechanism::SIMPLE:
			val = "SIMPLE";
			break;
		case SSD_Components::Caching_Mechanism::ADVANCED:
			val = "ADVANCED";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_Sharing_Mode";
	switch (Data_Cache_Sharing_Mode) {
		case SSD_Components::Cache_Sharing_Mode::SHARED:
			val = "SHARED";
			break;
		case SSD_Components::Cache_Sharing_Mode::EQUAL_PARTITIONING:
			val = "EQUAL_PARTITIONING";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_Capacity";
	val = std::to_string(Data_Cache_Capacity);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_DRAM_Row_Size";
	val = std::to_string(Data_Cache_DRAM_Row_Size);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_DRAM_Data_Rate";
	val = std::to_string(Data_Cache_DRAM_Data_Rate);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_DRAM_Data_Busrt_Size";
	val = std::to_string(Data_Cache_DRAM_Data_Busrt_Size);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_DRAM_tRCD";
	val = std::to_string(Data_Cache_DRAM_tRCD);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_DRAM_tCL";
	val = std::to_string(Data_Cache_DRAM_tCL);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Data_Cache_DRAM_tRP";
	val = std::to_string(Data_Cache_DRAM_tRP);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Address_Mapping";
	switch (Address_Mapping) {
		case SSD_Components::Flash_Address_Mapping_Type::PAGE_LEVEL:
			val = "PAGE_LEVEL";
			break;
		case SSD_Components::Flash_Address_Mapping_Type::HYBRID:
			val = "HYBRID";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Ideal_Mapping_Table";
	val = (Use_Copyback_for_GC ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);
	
	attr = "CMT_Capacity";
	val = std::to_string(CMT_Capacity);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "CMT_Sharing_Mode";
	switch (CMT_Sharing_Mode) {
		case SSD_Components::CMT_Sharing_Mode::SHARED:
			val = "SHARED";
			break;
		case SSD_Components::CMT_Sharing_Mode::EQUAL_SIZE_PARTITIONING:
			val = "EQUAL_SIZE_PARTITIONING";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Plane_Allocation_Scheme";
	switch (Plane_Allocation_Scheme) {
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::CDPW:
			val = "CDPW";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::CDWP:
			val = "CDWP";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::CPDW:
			val = "CPDW";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::CPWD:
			val = "CPWD";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::CWDP:
			val = "CWDP";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::CWPD:
			val = "CWPD";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::DCPW:
			val = "DCPW";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::DCWP:
			val = "DCWP";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::DPCW:
			val = "DPCW";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::DPWC:
			val = "DPWC";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::DWCP:
			val = "DWCP";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::DWPC:
			val = "DWPC";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::PCDW:
			val = "PCDW";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::PCWD:
			val = "PCWD";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::PDCW:
			val = "PDCW";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::PDWC:
			val = "PDWC";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::PWCD:
			val = "PWCD";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::PWDC:
			val = "PWDC";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::WCDP:
			val = "WCDP";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::WCPD:
			val = "WCPD";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::WDCP:
			val = "WDCP";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::WDPC:
			val = "WDPC";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::WPCD:
			val = "WPCD";
			break;
		case SSD_Components::Flash_Plane_Allocation_Scheme_Type::WPDC:
			val = "WPDC";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Transaction_Scheduling_Policy";
	switch (Transaction_Scheduling_Policy) {
		case SSD_Components::Flash_Scheduling_Type::OUT_OF_ORDER:
			val = "OUT_OF_ORDER";
			break;
		case SSD_Components::Flash_Scheduling_Type::PRIORITY_OUT_OF_ORDER:
			val = "PRIORITY_OUT_OF_ORDER";
			break;
		case SSD_Components::Flash_Scheduling_Type::FLIN:
			val = "FLIN";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Overprovisioning_Ratio";
	val = std::to_string(Overprovisioning_Ratio);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "GC_Exec_Threshold";
	val = std::to_string(GC_Exec_Threshold);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "GC_Block_Selection_Policy";
	switch (GC_Block_Selection_Policy) {
		case SSD_Components::GC_Block_Selection_Policy_Type::GREEDY:
			val = "GREEDY";
			break;
		case SSD_Components::GC_Block_Selection_Policy_Type::KV_THREE_GREEDY:
			val = "KV_THREE_GREEDY";
			break;
		case SSD_Components::GC_Block_Selection_Policy_Type::RGA:
			val = "RGA";
			break;
		case SSD_Components::GC_Block_Selection_Policy_Type::RANDOM:
			val = "RANDOM";
			break;
		case SSD_Components::GC_Block_Selection_Policy_Type::RANDOM_P:
			val = "RANDOM_P";
			break;
		case SSD_Components::GC_Block_Selection_Policy_Type::RANDOM_PP:
			val = "RANDOM_PP";
			break;
		case SSD_Components::GC_Block_Selection_Policy_Type::FIFO:
			val = "FIFO";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);
	
	attr = "Use_Copyback_for_GC";
	val = (Use_Copyback_for_GC ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Preemptible_GC_Enabled";
	val = (Preemptible_GC_Enabled ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "GC_Hard_Threshold";
	val = std::to_string(GC_Hard_Threshold);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Dynamic_Wearleveling_Enabled";
	val = (Dynamic_Wearleveling_Enabled ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Static_Wearleveling_Enabled";
	val = (Static_Wearleveling_Enabled ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);
	
	attr = "Static_Wearleveling_Threshold";
	val = std::to_string(Static_Wearleveling_Threshold);
	xmlwriter.Write_attribute_string(attr, val);
	
	attr = "Preferred_suspend_erase_time_for_read";
	val = std::to_string(Preferred_suspend_erase_time_for_read);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Preferred_suspend_erase_time_for_write";
	val = std::to_string(Preferred_suspend_erase_time_for_write);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Preferred_suspend_write_time_for_read";
	val = std::to_string(Preferred_suspend_write_time_for_read);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Flash_Channel_Count";
	val = std::to_string(Flash_Channel_Count);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Flash_Channel_Width";
	val = std::to_string(Flash_Channel_Width);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Channel_Transfer_Rate";
	val = std::to_string(Channel_Transfer_Rate);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Chip_No_Per_Channel";
	val = std::to_string(Chip_No_Per_Channel);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Flash_Comm_Protocol";
	switch (Flash_Comm_Protocol) {
		case SSD_Components::ONFI_Protocol::NVDDR2:
			val = "NVDDR2";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	Flash_Parameters.XML_serialize(xmlwriter);
	for (const auto& profile : Flash_Media_Profiles) profile.XML_serialize(xmlwriter);
	for (const auto& pool : Flash_Pools) pool.XML_serialize(xmlwriter);
	xmlwriter.Write_attribute_string("Measurement_Start_Time_Ns", std::to_string(Measurement_Start_Time_Ns));
	xmlwriter.Write_attribute_string("Measurement_End_Time_Ns", std::to_string(Measurement_End_Time_Ns));

	xmlwriter.Write_close_tag();
}

void Device_Parameter_Set::XML_deserialize(rapidxml::xml_node<> *node)
{
	try
	{
		for (auto param = node->first_node(); param; param = param->next_sibling()) {
			if (strcmp(param->name(), "Seed") == 0) {
				std::string val = param->value();
				Seed = std::stoi(val);
			} else if (strcmp(param->name(), "Enabled_Preconditioning") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Enabled_Preconditioning = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "Memory_Type") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "FLASH") == 0)
					Memory_Type = NVM::NVM_Type::FLASH;
				else PRINT_ERROR("Unknown NVM type specified in the SSD configuration file")
			} else if (strcmp(param->name(), "HostInterface_Type") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "NVME") == 0) {
					HostInterface_Type = HostInterface_Types::NVME;
				} else if (strcmp(val.c_str(), "SATA") == 0) {
					HostInterface_Type = HostInterface_Types::SATA;
				} else {
					PRINT_ERROR("Unknown host interface type specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "IO_Queue_Depth") == 0) {
				std::string val = param->value();
				IO_Queue_Depth = (uint16_t) std::stoull(val);
			} else if (strcmp(param->name(), "Queue_Fetch_Size") == 0) {
				std::string val = param->value();
				Queue_Fetch_Size = (uint16_t) std::stoull(val);
			} else if (strcmp(param->name(), "Caching_Mechanism") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "SIMPLE") == 0) {
					Caching_Mechanism = SSD_Components::Caching_Mechanism::SIMPLE;
				} else if (strcmp(val.c_str(), "ADVANCED") == 0) {
					Caching_Mechanism = SSD_Components::Caching_Mechanism::ADVANCED;
				} else {
					PRINT_ERROR("Unknown data caching mechanism specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "Data_Cache_Sharing_Mode") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "SHARED") == 0) {
					Data_Cache_Sharing_Mode = SSD_Components::Cache_Sharing_Mode::SHARED;
				} else if (strcmp(val.c_str(), "EQUAL_PARTITIONING") == 0) {
					Data_Cache_Sharing_Mode = SSD_Components::Cache_Sharing_Mode::EQUAL_PARTITIONING;
				} else {
					PRINT_ERROR("Unknown data cache sharing mode specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "Data_Cache_Capacity") == 0) {
				std::string val = param->value();
				Data_Cache_Capacity = std::stoul(val);
			} else if (strcmp(param->name(), "Data_Cache_DRAM_Row_Size") == 0) {
				std::string val = param->value();
				Data_Cache_DRAM_Row_Size = std::stoul(val);
			} else if (strcmp(param->name(), "Data_Cache_DRAM_Data_Rate") == 0) {
				std::string val = param->value();
				Data_Cache_DRAM_Data_Rate = std::stoul(val);
			} else if (strcmp(param->name(), "Data_Cache_DRAM_Data_Busrt_Size") == 0) {
				std::string val = param->value();
				Data_Cache_DRAM_Data_Busrt_Size = std::stoul(val);
			} else if (strcmp(param->name(), "Data_Cache_DRAM_tRCD") == 0) {
				std::string val = param->value();
				Data_Cache_DRAM_tRCD = std::stoul(val);
			} else if (strcmp(param->name(), "Data_Cache_DRAM_tCL") == 0) {
				std::string val = param->value();
				Data_Cache_DRAM_tCL = std::stoul(val);
			} else if (strcmp(param->name(), "Data_Cache_DRAM_tRP") == 0) {
				std::string val = param->value();
				Data_Cache_DRAM_tRP = std::stoul(val);
			} else if (strcmp(param->name(), "Address_Mapping") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "PAGE_LEVEL") == 0) {
					Address_Mapping = SSD_Components::Flash_Address_Mapping_Type::PAGE_LEVEL;
				} else if (strcmp(val.c_str(), "HYBRID") == 0) {
					Address_Mapping = SSD_Components::Flash_Address_Mapping_Type::HYBRID;
				} else {
					PRINT_ERROR("Unknown address mapping type specified in the SSD configuration file")
				}
			}
			else if (strcmp(param->name(), "Ideal_Mapping_Table") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Ideal_Mapping_Table = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "CMT_Capacity") == 0) {
				std::string val = param->value();
				CMT_Capacity = std::stoul(val);
			} else if (strcmp(param->name(), "CMT_Sharing_Mode") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "SHARED") == 0) {
					CMT_Sharing_Mode = SSD_Components::CMT_Sharing_Mode::SHARED;
				} else if (strcmp(val.c_str(), "EQUAL_PARTITIONING") == 0) {
					CMT_Sharing_Mode = SSD_Components::CMT_Sharing_Mode::EQUAL_SIZE_PARTITIONING;
				} else {
					PRINT_ERROR("Unknown CMT sharing mode specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "Plane_Allocation_Scheme") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "CDPW") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::CDPW;
				} else if (strcmp(val.c_str(), "CDWP") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::CDWP;
				} else if (strcmp(val.c_str(), "CPDW") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::CPDW;
				} else if (strcmp(val.c_str(), "CPWD") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::CPWD;
				} else if (strcmp(val.c_str(), "CWDP") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::CWDP;
				} else if (strcmp(val.c_str(), "CWPD") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::CWPD;
				} else if (strcmp(val.c_str(), "DCPW") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::DCPW;
				} else if (strcmp(val.c_str(), "DCWP") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::DCWP;
				} else if (strcmp(val.c_str(), "DPCW") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::DPCW;
				} else if (strcmp(val.c_str(), "DPWC") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::DPWC;
				} else if (strcmp(val.c_str(), "DWCP") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::DWCP;
				} else if (strcmp(val.c_str(), "DWPC") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::DWPC;
				} else if (strcmp(val.c_str(), "PCDW") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::PCDW;
				} else if (strcmp(val.c_str(), "PCWD") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::PCWD;
				} else if (strcmp(val.c_str(), "PDCW") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::PDCW;
				} else if (strcmp(val.c_str(), "PDWC") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::PDWC;
				} else if (strcmp(val.c_str(), "PWCD") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::PWCD;
				} else if (strcmp(val.c_str(), "PWDC") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::PWDC;
				} else if (strcmp(val.c_str(), "WCDP") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::WCDP;
				} else if (strcmp(val.c_str(), "WCPD") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::WCPD;
				} else if (strcmp(val.c_str(), "WDCP") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::WDCP;
				} else if (strcmp(val.c_str(), "WDPC") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::WDPC;
				} else if (strcmp(val.c_str(), "WPCD") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::WPCD;
				} else if (strcmp(val.c_str(), "WPDC") == 0) {
					Plane_Allocation_Scheme = SSD_Components::Flash_Plane_Allocation_Scheme_Type::WPDC;
				} else {
					PRINT_ERROR("Unknown plane allocation scheme type specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "Transaction_Scheduling_Policy") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "OUT_OF_ORDER") == 0) {
					Transaction_Scheduling_Policy = SSD_Components::Flash_Scheduling_Type::OUT_OF_ORDER;
				}
				else if (strcmp(val.c_str(), "PRIORITY_OUT_OF_ORDER") == 0)
				{
					Transaction_Scheduling_Policy = SSD_Components::Flash_Scheduling_Type::PRIORITY_OUT_OF_ORDER;
				}
				else if (strcmp(val.c_str(), "FLIN") == 0)
				{
					Transaction_Scheduling_Policy = SSD_Components::Flash_Scheduling_Type::FLIN;
				} else {
					PRINT_ERROR("Unknown transaction scheduling type specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "Overprovisioning_Ratio") == 0) {
				std::string val = param->value();
				Overprovisioning_Ratio = std::stod(val);
				if(Overprovisioning_Ratio < 0.05) {
					PRINT_MESSAGE("The specified overprovisioning ratio is too small. The simluation may not run correctly.")
				}
			} else if (strcmp(param->name(), "GC_Exec_Threshold") == 0) {
				std::string val = param->value();
				GC_Exec_Threshold = std::stod(val);
			} else if (strcmp(param->name(), "GC_Block_Selection_Policy") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "GREEDY") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::GREEDY;
				} else if (strcmp(val.c_str(), "KV_THREE_GREEDY") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::KV_THREE_GREEDY;
				} else if (strcmp(val.c_str(), "RGA") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::RGA;
				} else if (strcmp(val.c_str(), "RANDOM") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::RANDOM;
				} else if (strcmp(val.c_str(), "RANDOM_P") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::RANDOM_P;
				} else if (strcmp(val.c_str(), "RANDOM_PP") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::RANDOM_PP;
				} else if (strcmp(val.c_str(), "FIFO") == 0) {
					GC_Block_Selection_Policy = SSD_Components::GC_Block_Selection_Policy_Type::FIFO;
				} else {
					PRINT_ERROR("Unknown GC block selection policy specified in the SSD configuration file")
				}
			} else if (strcmp(param->name(), "Use_Copyback_for_GC") == 0) {
					std::string val = param->value();
					std::transform(val.begin(), val.end(), val.begin(), ::toupper);
					Use_Copyback_for_GC = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "Preemptible_GC_Enabled") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Preemptible_GC_Enabled = (val.compare("FALSE") == 0? false : true);
			} else if (strcmp(param->name(), "GC_Hard_Threshold") == 0) {
				std::string val = param->value();
				GC_Hard_Threshold = std::stod(val);
			} else if (strcmp(param->name(), "Dynamic_Wearleveling_Enabled") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Dynamic_Wearleveling_Enabled = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "Static_Wearleveling_Enabled") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Static_Wearleveling_Enabled = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "Static_Wearleveling_Threshold") == 0) {
				std::string val = param->value();
				Static_Wearleveling_Threshold = std::stoul(val);
			} else if (strcmp(param->name(), "Prefered_suspend_erase_time_for_read") == 0) {
				std::string val = param->value();
				Preferred_suspend_erase_time_for_read = std::stoull(val);
			} else if (strcmp(param->name(), "Preferred_suspend_erase_time_for_write") == 0) {
				std::string val = param->value();
				Preferred_suspend_erase_time_for_write = std::stoull(val);
			} else if (strcmp(param->name(), "Preferred_suspend_write_time_for_read") == 0) {
				std::string val = param->value();
				Preferred_suspend_write_time_for_read = std::stoull(val);
			} else if (strcmp(param->name(), "Flash_Channel_Count") == 0) {
				std::string val = param->value();
				Flash_Channel_Count = std::stoul(val);
			} else if (strcmp(param->name(), "Flash_Channel_Width") == 0) {
				std::string val = param->value();
				Flash_Channel_Width = std::stoul(val);
			} else if (strcmp(param->name(), "Channel_Transfer_Rate") == 0) {
				std::string val = param->value();
				Channel_Transfer_Rate = std::stoul(val);
			} else if (strcmp(param->name(), "Chip_No_Per_Channel") == 0) {
				std::string val = param->value();
				Chip_No_Per_Channel = std::stoul(val);
			} else if (strcmp(param->name(), "Flash_Comm_Protocol") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "NVDDR2") == 0) {
					Flash_Comm_Protocol = SSD_Components::ONFI_Protocol::NVDDR2;
				} else {
					PRINT_ERROR("Unknown flash communication protocol type specified in the SSD configuration file")
				}
			}
			else if (strcmp(param->name(), "Flash_Parameter_Set") == 0)
			{
				Flash_Parameters.XML_deserialize(param);
			} else if (strcmp(param->name(), "Flash_Media_Profile") == 0) {
				Flash_Media_Profile profile;
				profile.XML_deserialize(param);
				Flash_Media_Profiles.push_back(profile);
			} else if (strcmp(param->name(), "Flash_Pool_Parameter_Set") == 0) {
				Flash_Pool_Parameter_Set pool;
				pool.XML_deserialize(param);
				Flash_Pools.push_back(pool);
			} else if (strcmp(param->name(), "Measurement_Start_Time_Ns") == 0) {
				Measurement_Start_Time_Ns = std::stoull(param->value());
			} else if (strcmp(param->name(), "Measurement_End_Time_Ns") == 0) {
				Measurement_End_Time_Ns = std::stoull(param->value());
			}
		}
	}
	catch (...)
	{
		PRINT_ERROR("Error in Device_Parameter_Set!")
	}
}
