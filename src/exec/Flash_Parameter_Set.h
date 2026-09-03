#ifndef FLASH_PARAMETER_SET_H
#define FLASH_PARAMETER_SET_H

#include "../sim/Sim_Defs.h"
#include "../nvm_chip/flash_memory/FlashTypes.h"
#include "Parameter_Set_Base.h"

class Flash_Parameter_Set : Parameter_Set_Base
{
public:
	Flash_Parameter_Set();
	unsigned int Die_No_Per_Chip;
	unsigned int Plane_No_Per_Die;
	unsigned int Block_No_Per_Plane;
	unsigned int Page_No_Per_Block;
	unsigned int Page_Capacity;
	unsigned int Page_Metadat_Capacity;
	void XML_serialize(Utils::XmlWriter& xmlwriter);
	void XML_deserialize(rapidxml::xml_node<> *node);
};

#endif // !FLASH_PARAMETER_SET_H
