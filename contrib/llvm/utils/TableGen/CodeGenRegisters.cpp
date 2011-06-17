//===- CodeGenRegisters.cpp - Register and RegisterClass Info -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines structures to encapsulate information gleaned from the
// target register and register class definitions.
//
//===----------------------------------------------------------------------===//

#include "CodeGenRegisters.h"
#include "CodeGenTarget.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
//                              CodeGenRegister
//===----------------------------------------------------------------------===//

CodeGenRegister::CodeGenRegister(Record *R, unsigned Enum)
  : TheDef(R),
    EnumValue(Enum),
    CostPerUse(R->getValueAsInt("CostPerUse")),
    SubRegsComplete(false)
{}

const std::string &CodeGenRegister::getName() const {
  return TheDef->getName();
}

namespace {
  struct Orphan {
    CodeGenRegister *SubReg;
    Record *First, *Second;
    Orphan(CodeGenRegister *r, Record *a, Record *b)
      : SubReg(r), First(a), Second(b) {}
  };
}

const CodeGenRegister::SubRegMap &
CodeGenRegister::getSubRegs(CodeGenRegBank &RegBank) {
  // Only compute this map once.
  if (SubRegsComplete)
    return SubRegs;
  SubRegsComplete = true;

  std::vector<Record*> SubList = TheDef->getValueAsListOfDefs("SubRegs");
  std::vector<Record*> Indices = TheDef->getValueAsListOfDefs("SubRegIndices");
  if (SubList.size() != Indices.size())
    throw TGError(TheDef->getLoc(), "Register " + getName() +
                  " SubRegIndices doesn't match SubRegs");

  // First insert the direct subregs and make sure they are fully indexed.
  for (unsigned i = 0, e = SubList.size(); i != e; ++i) {
    CodeGenRegister *SR = RegBank.getReg(SubList[i]);
    if (!SubRegs.insert(std::make_pair(Indices[i], SR)).second)
      throw TGError(TheDef->getLoc(), "SubRegIndex " + Indices[i]->getName() +
                    " appears twice in Register " + getName());
  }

  // Keep track of inherited subregs and how they can be reached.
  SmallVector<Orphan, 8> Orphans;

  // Clone inherited subregs and place duplicate entries on Orphans.
  // Here the order is important - earlier subregs take precedence.
  for (unsigned i = 0, e = SubList.size(); i != e; ++i) {
    CodeGenRegister *SR = RegBank.getReg(SubList[i]);
    const SubRegMap &Map = SR->getSubRegs(RegBank);
    for (SubRegMap::const_iterator SI = Map.begin(), SE = Map.end(); SI != SE;
         ++SI)
      if (!SubRegs.insert(*SI).second)
        Orphans.push_back(Orphan(SI->second, Indices[i], SI->first));
  }

  // Process the composites.
  ListInit *Comps = TheDef->getValueAsListInit("CompositeIndices");
  for (unsigned i = 0, e = Comps->size(); i != e; ++i) {
    DagInit *Pat = dynamic_cast<DagInit*>(Comps->getElement(i));
    if (!Pat)
      throw TGError(TheDef->getLoc(), "Invalid dag '" +
                    Comps->getElement(i)->getAsString() +
                    "' in CompositeIndices");
    DefInit *BaseIdxInit = dynamic_cast<DefInit*>(Pat->getOperator());
    if (!BaseIdxInit || !BaseIdxInit->getDef()->isSubClassOf("SubRegIndex"))
      throw TGError(TheDef->getLoc(), "Invalid SubClassIndex in " +
                    Pat->getAsString());

    // Resolve list of subreg indices into R2.
    CodeGenRegister *R2 = this;
    for (DagInit::const_arg_iterator di = Pat->arg_begin(),
         de = Pat->arg_end(); di != de; ++di) {
      DefInit *IdxInit = dynamic_cast<DefInit*>(*di);
      if (!IdxInit || !IdxInit->getDef()->isSubClassOf("SubRegIndex"))
        throw TGError(TheDef->getLoc(), "Invalid SubClassIndex in " +
                      Pat->getAsString());
      const SubRegMap &R2Subs = R2->getSubRegs(RegBank);
      SubRegMap::const_iterator ni = R2Subs.find(IdxInit->getDef());
      if (ni == R2Subs.end())
        throw TGError(TheDef->getLoc(), "Composite " + Pat->getAsString() +
                      " refers to bad index in " + R2->getName());
      R2 = ni->second;
    }

    // Insert composite index. Allow overriding inherited indices etc.
    SubRegs[BaseIdxInit->getDef()] = R2;

    // R2 is no longer an orphan.
    for (unsigned j = 0, je = Orphans.size(); j != je; ++j)
      if (Orphans[j].SubReg == R2)
          Orphans[j].SubReg = 0;
  }

  // Now Orphans contains the inherited subregisters without a direct index.
  // Create inferred indexes for all missing entries.
  for (unsigned i = 0, e = Orphans.size(); i != e; ++i) {
    Orphan &O = Orphans[i];
    if (!O.SubReg)
      continue;
    SubRegs[RegBank.getCompositeSubRegIndex(O.First, O.Second, true)] =
      O.SubReg;
  }
  return SubRegs;
}

//===----------------------------------------------------------------------===//
//                            CodeGenRegisterClass
//===----------------------------------------------------------------------===//

CodeGenRegisterClass::CodeGenRegisterClass(Record *R) : TheDef(R) {
  // Rename anonymous register classes.
  if (R->getName().size() > 9 && R->getName()[9] == '.') {
    static unsigned AnonCounter = 0;
    R->setName("AnonRegClass_"+utostr(AnonCounter++));
  }

  std::vector<Record*> TypeList = R->getValueAsListOfDefs("RegTypes");
  for (unsigned i = 0, e = TypeList.size(); i != e; ++i) {
    Record *Type = TypeList[i];
    if (!Type->isSubClassOf("ValueType"))
      throw "RegTypes list member '" + Type->getName() +
        "' does not derive from the ValueType class!";
    VTs.push_back(getValueType(Type));
  }
  assert(!VTs.empty() && "RegisterClass must contain at least one ValueType!");

  std::vector<Record*> RegList = R->getValueAsListOfDefs("MemberList");
  for (unsigned i = 0, e = RegList.size(); i != e; ++i) {
    Record *Reg = RegList[i];
    if (!Reg->isSubClassOf("Register"))
      throw "Register Class member '" + Reg->getName() +
            "' does not derive from the Register class!";
    Elements.push_back(Reg);
  }

  // SubRegClasses is a list<dag> containing (RC, subregindex, ...) dags.
  ListInit *SRC = R->getValueAsListInit("SubRegClasses");
  for (ListInit::const_iterator i = SRC->begin(), e = SRC->end(); i != e; ++i) {
    DagInit *DAG = dynamic_cast<DagInit*>(*i);
    if (!DAG) throw "SubRegClasses must contain DAGs";
    DefInit *DAGOp = dynamic_cast<DefInit*>(DAG->getOperator());
    Record *RCRec;
    if (!DAGOp || !(RCRec = DAGOp->getDef())->isSubClassOf("RegisterClass"))
      throw "Operator '" + DAG->getOperator()->getAsString() +
        "' in SubRegClasses is not a RegisterClass";
    // Iterate over args, all SubRegIndex instances.
    for (DagInit::const_arg_iterator ai = DAG->arg_begin(), ae = DAG->arg_end();
         ai != ae; ++ai) {
      DefInit *Idx = dynamic_cast<DefInit*>(*ai);
      Record *IdxRec;
      if (!Idx || !(IdxRec = Idx->getDef())->isSubClassOf("SubRegIndex"))
        throw "Argument '" + (*ai)->getAsString() +
          "' in SubRegClasses is not a SubRegIndex";
      if (!SubRegClasses.insert(std::make_pair(IdxRec, RCRec)).second)
        throw "SubRegIndex '" + IdxRec->getName() + "' mentioned twice";
    }
  }

  // Allow targets to override the size in bits of the RegisterClass.
  unsigned Size = R->getValueAsInt("Size");

  Namespace = R->getValueAsString("Namespace");
  SpillSize = Size ? Size : EVT(VTs[0]).getSizeInBits();
  SpillAlignment = R->getValueAsInt("Alignment");
  CopyCost = R->getValueAsInt("CopyCost");
  Allocatable = R->getValueAsBit("isAllocatable");
  MethodBodies = R->getValueAsCode("MethodBodies");
  MethodProtos = R->getValueAsCode("MethodProtos");
}

const std::string &CodeGenRegisterClass::getName() const {
  return TheDef->getName();
}

//===----------------------------------------------------------------------===//
//                               CodeGenRegBank
//===----------------------------------------------------------------------===//

CodeGenRegBank::CodeGenRegBank(RecordKeeper &Records) : Records(Records) {
  // Read in the user-defined (named) sub-register indices.
  // More indices will be synthesized later.
  SubRegIndices = Records.getAllDerivedDefinitions("SubRegIndex");
  std::sort(SubRegIndices.begin(), SubRegIndices.end(), LessRecord());
  NumNamedIndices = SubRegIndices.size();

  // Read in the register definitions.
  std::vector<Record*> Regs = Records.getAllDerivedDefinitions("Register");
  std::sort(Regs.begin(), Regs.end(), LessRecord());
  Registers.reserve(Regs.size());
  // Assign the enumeration values.
  for (unsigned i = 0, e = Regs.size(); i != e; ++i)
    Registers.push_back(CodeGenRegister(Regs[i], i + 1));
}

CodeGenRegister *CodeGenRegBank::getReg(Record *Def) {
  if (Def2Reg.empty())
    for (unsigned i = 0, e = Registers.size(); i != e; ++i)
      Def2Reg[Registers[i].TheDef] = &Registers[i];

  if (CodeGenRegister *Reg = Def2Reg[Def])
    return Reg;

  throw TGError(Def->getLoc(), "Not a known Register!");
}

Record *CodeGenRegBank::getCompositeSubRegIndex(Record *A, Record *B,
                                                bool create) {
  // Look for an existing entry.
  Record *&Comp = Composite[std::make_pair(A, B)];
  if (Comp || !create)
    return Comp;

  // None exists, synthesize one.
  std::string Name = A->getName() + "_then_" + B->getName();
  Comp = new Record(Name, SMLoc(), Records);
  Records.addDef(Comp);
  SubRegIndices.push_back(Comp);
  return Comp;
}

unsigned CodeGenRegBank::getSubRegIndexNo(Record *idx) {
  std::vector<Record*>::const_iterator i =
    std::find(SubRegIndices.begin(), SubRegIndices.end(), idx);
  assert(i != SubRegIndices.end() && "Not a SubRegIndex");
  return (i - SubRegIndices.begin()) + 1;
}

void CodeGenRegBank::computeComposites() {
  // Precompute all sub-register maps. This will create Composite entries for
  // all inferred sub-register indices.
  for (unsigned i = 0, e = Registers.size(); i != e; ++i)
    Registers[i].getSubRegs(*this);

  for (unsigned i = 0, e = Registers.size(); i != e; ++i) {
    CodeGenRegister *Reg1 = &Registers[i];
    const CodeGenRegister::SubRegMap &SRM1 = Reg1->getSubRegs(*this);
    for (CodeGenRegister::SubRegMap::const_iterator i1 = SRM1.begin(),
         e1 = SRM1.end(); i1 != e1; ++i1) {
      Record *Idx1 = i1->first;
      CodeGenRegister *Reg2 = i1->second;
      // Ignore identity compositions.
      if (Reg1 == Reg2)
        continue;
      const CodeGenRegister::SubRegMap &SRM2 = Reg2->getSubRegs(*this);
      // Try composing Idx1 with another SubRegIndex.
      for (CodeGenRegister::SubRegMap::const_iterator i2 = SRM2.begin(),
           e2 = SRM2.end(); i2 != e2; ++i2) {
        std::pair<Record*, Record*> IdxPair(Idx1, i2->first);
        CodeGenRegister *Reg3 = i2->second;
        // Ignore identity compositions.
        if (Reg2 == Reg3)
          continue;
        // OK Reg1:IdxPair == Reg3. Find the index with Reg:Idx == Reg3.
        for (CodeGenRegister::SubRegMap::const_iterator i1d = SRM1.begin(),
             e1d = SRM1.end(); i1d != e1d; ++i1d) {
          if (i1d->second == Reg3) {
            std::pair<CompositeMap::iterator, bool> Ins =
              Composite.insert(std::make_pair(IdxPair, i1d->first));
            // Conflicting composition? Emit a warning but allow it.
            if (!Ins.second && Ins.first->second != i1d->first) {
              errs() << "Warning: SubRegIndex " << getQualifiedName(Idx1)
                     << " and " << getQualifiedName(IdxPair.second)
                     << " compose ambiguously as "
                     << getQualifiedName(Ins.first->second) << " or "
                     << getQualifiedName(i1d->first) << "\n";
            }
          }
        }
      }
    }
  }

  // We don't care about the difference between (Idx1, Idx2) -> Idx2 and invalid
  // compositions, so remove any mappings of that form.
  for (CompositeMap::iterator i = Composite.begin(), e = Composite.end();
       i != e;) {
    CompositeMap::iterator j = i;
    ++i;
    if (j->first.second == j->second)
      Composite.erase(j);
  }
}

void CodeGenRegBank::computeDerivedInfo() {
  computeComposites();
}
