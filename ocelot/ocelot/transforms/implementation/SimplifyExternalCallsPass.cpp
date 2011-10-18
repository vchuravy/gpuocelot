/*! \file   SimplifyExternalCallsPass.cpp
	\date   Saturday April 9, 2011
	\author Gregory Diamos <gregory.diamos@gatech.edu>
	\brief  The source file for the SimplifyExternalCallsPass class.
*/

// Ocelot Includes
#include <ocelot/transforms/interface/SimplifyExternalCallsPass.h>

#include <ocelot/analysis/interface/DataflowGraph.h>

#include <ocelot/ir/interface/ExternalFunctionSet.h>
#include <ocelot/ir/interface/Module.h>

// Hydrazine Includes
#include <hydrazine/implementation/debug.h>

// Preprocessor Macros
#ifdef REPORT_BASE
#undef REPORT_BASE
#endif

#define REPORT_BASE 0

namespace transforms
{

static void simplifyCall(ir::PTXKernel& kernel,
	ir::ControlFlowGraph::iterator block,
	ir::BasicBlock::InstructionList::iterator callIterator,
	analysis::DataflowGraph& dfg)
{
	typedef std::unordered_set<std::string> StringSet;
	typedef std::unordered_map<std::string,
		ir::PTXOperand::RegisterType> RegisterMap;
	typedef std::vector<ir::BasicBlock::InstructionList::iterator>
		InstructionVector;

	ir::PTXInstruction& call = static_cast<ir::PTXInstruction&>(**callIterator);

	// Get the names of parameters
	StringSet parameterNames;
	StringSet inputNames;
	StringSet outputNames;
	
	report("  return arguments:");
	
	for(ir::PTXOperand::Array::const_iterator parameter = call.d.array.begin();
		parameter != call.d.array.end(); ++parameter)
	{
		if(parameter->addressMode != ir::PTXOperand::Address) continue;
		report("   " << parameter->identifier << " ("
		    << ir::PTXOperand::toString(parameter->addressMode) << ")");
		parameterNames.insert(parameter->identifier);
		outputNames.insert(parameter->identifier);
	}

	report("  input arguments:");
	for(ir::PTXOperand::Array::const_iterator parameter = call.b.array.begin();
		parameter != call.b.array.end(); ++parameter)
	{
		if(parameter->addressMode != ir::PTXOperand::Address) continue;
		report("   " << parameter->identifier << " ("
		    << ir::PTXOperand::toString(parameter->addressMode) << ")");
		parameterNames.insert(parameter->identifier);
		inputNames.insert(parameter->identifier);
	}
	
	// Find the registers that are mapped to these parameters
	RegisterMap nameToRegister;
	InstructionVector killList;
	
	report("  searching for argument accesses");
	for(ir::BasicBlock::InstructionList::reverse_iterator instruction =
		ir::BasicBlock::InstructionList::reverse_iterator(callIterator);
		instruction != block->instructions.rend(); ++instruction)
	{
		ir::PTXInstruction& ptx = static_cast<ir::PTXInstruction&>(
			**instruction);
	
		if(ptx.opcode == ir::PTXInstruction::St)
		{
			if(ptx.addressSpace == ir::PTXInstruction::Param)
			{
				if(ptx.d.addressMode == ir::PTXOperand::Address)
				{
					StringSet::iterator input =
						inputNames.find(ptx.d.identifier);
					
					if(inputNames.end() != input)
					{
						report("   found input '" << ptx.d.identifier << "'");
	
						if(ptx.a.addressMode == ir::PTXOperand::Register)
						{
							assert(nameToRegister.count(ptx.d.identifier) == 0);
						
							// if the types match, kill the store
							if(ptx.type == ptx.a.type)
							{
								nameToRegister.insert(std::make_pair(
									ptx.d.identifier, ptx.a.reg));
								killList.push_back(--instruction.base());
							}
							else
							{
								// otherwise, convert it into a cast
								ir::PTXOperand temp = ir::PTXOperand(
									ir::PTXOperand::Register, ptx.type,
									dfg.newRegister());

								nameToRegister.insert(std::make_pair(
									ptx.d.identifier, temp.reg));
						
								ptx.opcode = ir::PTXInstruction::Cvt;
								ptx.d = temp;
								ptx.modifier =
									ir::PTXInstruction::Modifier_invalid;
							}
						}
						else
						{
							assert(ptx.a.addressMode
								== ir::PTXOperand::Immediate);
						
							// handle immediate operands
							ir::PTXOperand temp = ir::PTXOperand(
								ir::PTXOperand::Register, ptx.type,
								dfg.newRegister());

							nameToRegister.insert(std::make_pair(
								ptx.d.identifier, temp.reg));
					
							ptx.opcode = ir::PTXInstruction::Mov;
							ptx.d = temp;
						}
						
						inputNames.erase(input);
					}
				}
			}
		}
	}
	
	for(ir::BasicBlock::InstructionList::iterator instruction = callIterator;
		instruction != block->instructions.end(); ++instruction)
	{
		ir::PTXInstruction& ptx = static_cast<ir::PTXInstruction&>(
			**instruction);
	
		if(ptx.opcode == ir::PTXInstruction::Ld)
		{
			if(ptx.addressSpace == ir::PTXInstruction::Param)
			{
				if(ptx.a.addressMode == ir::PTXOperand::Address)
				{
					StringSet::iterator output =
						outputNames.find(ptx.a.identifier);
					
					if(outputNames.end() != output)
					{
						report("   found output '" << ptx.a.identifier << "'");
	
						assert(ptx.d.addressMode == ir::PTXOperand::Register);
						assert(nameToRegister.count(ptx.a.identifier) == 0);
						// if the types match, kill the load
						if(ptx.type == ptx.d.type)
						{
							nameToRegister.insert(std::make_pair(
								ptx.a.identifier, ptx.d.reg));
							killList.push_back(instruction);
						}
						else
						{
							// otherwise, convert it into a cast
							ir::PTXOperand temp = ir::PTXOperand(
								ir::PTXOperand::Register, ptx.type,
								dfg.newRegister());

							nameToRegister.insert(std::make_pair(
								ptx.a.identifier, temp.reg));
						
							ptx.opcode = ir::PTXInstruction::Cvt;
							ptx.a = temp;
							ptx.modifier =
								ir::PTXInstruction::Modifier_invalid;
						}
						
						outputNames.erase(output);
					}
				}
			}
		}
	}
	
	// Modify the call to replace parameter operands with register operands
	for(ir::PTXOperand::Array::iterator parameter = call.d.array.begin();
		parameter != call.d.array.end(); ++parameter)
	{
		if(parameter->addressMode != ir::PTXOperand::Address) continue;
		RegisterMap::iterator mapping = nameToRegister.find(
			parameter->identifier);
		assertM(mapping != nameToRegister.end(),
			"Could not find register source of operand "
			<< parameter->identifier);
		
		parameter->addressMode = ir::PTXOperand::Register;
		parameter->reg         = mapping->second;

		parameter->identifier.clear();
	}

	for(ir::PTXOperand::Array::iterator parameter = call.b.array.begin();
		parameter != call.b.array.end(); ++parameter)
	{
		if(parameter->addressMode != ir::PTXOperand::Address) continue;
		RegisterMap::iterator mapping = nameToRegister.find(
			parameter->identifier);
		assert(mapping != nameToRegister.end());
		
		parameter->addressMode = ir::PTXOperand::Register;
		parameter->reg         = mapping->second;

		parameter->identifier.clear();
	}
	
	report("  new call is '" << call.toString() << "'");
	
	// Remove the parameter instructions
	report("  removing ld/st param instructions:");
	for(InstructionVector::const_iterator killed = killList.begin();
		killed != killList.end(); ++killed)
	{
		report("   removing " << (**killed)->toString());
		block->instructions.erase(*killed);
	}
	
	// Remove the parameters from the kernels
	report("  removing parameters:");
	for(StringSet::const_iterator parameterName = parameterNames.begin();
		parameterName != parameterNames.end(); ++parameterName)
	{
		report("   " << *parameterName);
		ir::Kernel::ParameterMap::iterator
			parameter = kernel.parameters.find(*parameterName);
			
		// we may have already erased the parameter
		if(parameter != kernel.parameters.end())
		{
			kernel.parameters.erase(parameter);
		}
	}
}

SimplifyExternalCallsPass::SimplifyExternalCallsPass(
	const ir::ExternalFunctionSet& e, bool s) 
: KernelPass(analysis::Analysis::DataflowGraphAnalysis,
	"SimplifyExternalCallsPass"), _externals(&e), _simplifyAll(s)
{

}

void SimplifyExternalCallsPass::initialize(const ir::Module& m)
{

}

void SimplifyExternalCallsPass::runOnKernel(ir::IRKernel& k)
{
	ir::PTXKernel& kernel = static_cast<ir::PTXKernel&>(k);

	Analysis* analysis = getAnalysis(Analysis::DataflowGraphAnalysis);
	assert(analysis != 0);
	
	analysis::DataflowGraph* dfg =
		static_cast<analysis::DataflowGraph*>(analysis);

	report("Running SimplifyExternalCallsPass on kernel '" + k.name + "'");

	for(ir::ControlFlowGraph::iterator block = k.cfg()->begin();
		block != k.cfg()->end(); ++block)
	{
		for(ir::BasicBlock::InstructionList::iterator
			instruction = block->instructions.begin();
			instruction != block->instructions.end(); ++instruction)
		{
			ir::PTXInstruction& ptx = static_cast<ir::PTXInstruction&>(
				**instruction);
		
			if(ptx.opcode == ir::PTXInstruction::Call)
			{
				if(_simplifyAll)
				{
					report(" For " << ptx.toString());
					simplifyCall(kernel, block, instruction, *dfg);
				}
				else if(k.module->kernels().count(ptx.a.identifier) == 0)
				{
					if(_externals->find(ptx.a.identifier) != 0)
					{
						report(" For " << ptx.toString());
						simplifyCall(kernel, block, instruction, *dfg);
					}
				}
			}
		}
	}
}

void SimplifyExternalCallsPass::finalize()
{

}

}

