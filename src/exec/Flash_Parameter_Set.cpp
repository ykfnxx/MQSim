#include <string.h>
#include "../sim/Engine.h"
#include "Flash_Parameter_Set.h"

Flash_Parameter_Set::Flash_Parameter_Set()
	: Die_No_Per_Chip(2), Plane_No_Per_Die(2), Block_No_Per_Plane(2048),
	  Page_No_Per_Block(256), Page_Capacity(8192), Page_Metadat_Capacity(1872)
{
}

void Flash_Parameter_Set::XML_serialize(Utils::XmlWriter& xmlwriter)
{
	std::string tmp;
	tmp = "Flash_Parameter_Set";
	xmlwriter.Write_open_tag(tmp);

	std::string attr = "Die_No_Per_Chip";
	std::string val = std::to_string(Die_No_Per_Chip);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Plane_No_Per_Die";
	val = std::to_string(Plane_No_Per_Die);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Block_No_Per_Plane";
	val = std::to_string(Block_No_Per_Plane);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Page_No_Per_Block";
	val = std::to_string(Page_No_Per_Block);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Page_Capacity";
	val = std::to_string(Page_Capacity);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Page_Metadat_Capacity";
	val = std::to_string(Page_Metadat_Capacity);
	xmlwriter.Write_attribute_string(attr, val);

	xmlwriter.Write_close_tag();
}

void Flash_Parameter_Set::XML_deserialize(rapidxml::xml_node<> *node)
{
	try {
		for (auto param = node->first_node(); param; param = param->next_sibling()) {
			if (strcmp(param->name(), "Die_No_Per_Chip") == 0) {
				std::string val = param->value();
				Die_No_Per_Chip = std::stoul(val);
			} else if (strcmp(param->name(), "Plane_No_Per_Die") == 0) {
				std::string val = param->value();
				Plane_No_Per_Die = std::stoul(val);
			} else if (strcmp(param->name(), "Block_No_Per_Plane") == 0) {
				std::string val = param->value();
				Block_No_Per_Plane = std::stoul(val);
			} else if (strcmp(param->name(), "Page_No_Per_Block") == 0) {
				std::string val = param->value();
				Page_No_Per_Block = std::stoul(val);
			} else if (strcmp(param->name(), "Page_Capacity") == 0) {
				std::string val = param->value();
				Page_Capacity = std::stoul(val);
			} else if (strcmp(param->name(), "Page_Metadat_Capacity") == 0) {
				std::string val = param->value();
				Page_Metadat_Capacity = std::stoul(val);
			} else {
				PRINT_ERROR("Flash_Parameter_Set accepts geometry only; move media field " << param->name() << " to Flash_Media_Profile")
			}
		}
	} catch (...) {
		PRINT_ERROR("Error in the Flash_Parameter_Set!")
	}
}
