//
//  Copyright (C) 2004-2025 Greg Landrum and other RDKit contributors
//
//   @@ All Rights Reserved @@
//  This file is part of the RDKit.
//  The contents are covered by the terms of the BSD license
//  which is included in the file license.txt, found at the root
//  of the RDKit source tree.
//
#include <GraphMol/RDKitBase.h>
#include <GraphMol/Chirality.h>
#include <DistGeom/BoundsMatrix.h>
#include "BoundsMatrixBuilder.h"
#include <GraphMol/ForceFieldHelpers/UFF/AtomTyper.h>
#include <ForceField/UFF/BondStretch.h>
#include <Geometry/Utils.h>

#include <RDGeneral/utils.h>
#include <RDGeneral/RDLog.h>
#include <RDGeneral/Exceptions.h>
#include <Numerics/SymmMatrix.h>
#include <DistGeom/TriangleSmooth.h>
#include <boost/dynamic_bitset.hpp>
#include <algorithm>
#include <unordered_set>

const double DIST12_DELTA = 0.01;
// const double ANGLE_DELTA = 0.0837;
// const double RANGLE_DELTA = 0.0837; // tolerance for bond angles
// const double TANGLE_DELTA = 0.0837; // tolerance for torsion angle
const double DIST13_TOL = 0.04;
const double GEN_DIST_TOL = 0.06;  //  a general distance tolerance
const double DIST15_TOL = 0.08;
const double VDW_SCALE_15 = 0.7;
const double MAX_UPPER = 1000.0;
static const double minMacrocycleRingSize = 9;
#include <map>

namespace RDKit {
namespace DGeomHelpers {
// forward declarations:
typedef boost::shared_ptr<RDNumeric::IntSymmMatrix> SymmIntMatPtr;
typedef boost::shared_ptr<RDNumeric::DoubleSymmMatrix> SymmDoubleMatPtr;

typedef boost::dynamic_bitset<> BIT_SET;

//! Bunch of functions to set distance bound based on topology

typedef std::map<int, double> INT_DOUBLE_MAP;
typedef INT_DOUBLE_MAP::const_iterator INT_DOUBLE_MAP_CI;

typedef std::vector<long int> LINT_VECT;

//! A structure used to store planar 14 paths - cis/trans
struct Path14Configuration {
  unsigned int bid1, bid2, bid3;
  typedef enum {
    CIS = 0,
    TRANS,
    OTHER,
  } Path14Type;
  Path14Type type;
};

typedef std::vector<Path14Configuration> PATH14_VECT;
typedef PATH14_VECT::iterator PATH14_VECT_I;
typedef PATH14_VECT::const_iterator PATH14_VECT_CI;

class ComputedData {
 public:
  ComputedData(unsigned int nAtoms, unsigned int nBonds) {
    bondLengths.resize(nBonds);
    auto *bAdj = new RDNumeric::IntSymmMatrix(nBonds, -1);
    bondAdj.reset(bAdj);
    auto *bAngles = new RDNumeric::DoubleSymmMatrix(nBonds, -1.0);
    bondAngles.reset(bAngles);
    set15Atoms.resize(nAtoms * nAtoms);
  }

  ~ComputedData() = default;

  DOUBLE_VECT bondLengths;
  SymmIntMatPtr bondAdj;  // bond adjacency matrix
  SymmDoubleMatPtr bondAngles;
  PATH14_VECT paths14;
  std::unordered_set<std::uint64_t> cisPaths;
  std::unordered_set<std::uint64_t> transPaths;
  BIT_SET set15Atoms;
};

//! Set 1-2 distance bounds for between atoms in a molecule
/*!
  These are mostly bond lengths obtained from UFF parameters and then
  adjusted by a small tolerance to set the upper and lower limits
  \param mol          The molecule of interest
  \param mmat         Bounds matrix to which the bounds are written
  \param accumData    Used to store the data that have been calculated so far
                      about the molecule
*/
void set12Bounds(const ROMol &mol, DistGeom::BoundsMatPtr mmat,
                 ComputedData &accumData);

//! Set 1-3 distance bounds for atoms in a molecule
/*!
  These are computed using bond angles and bond lengths. There are special
  cases here, in particular for 3, 4, and 5 membered rings. Special attention
  is also paid to fused ring ring systems, when setting bounds on atoms that
  have an atom between that is shared by multiple rings.
  \param mol          Molecule of interest
  \param mmat         Bounds matrix to which the bounds are written
  \param accumData    Used to store the data that have been calculated so far
                      about the molecule
  <b>Procedure</b>
  All 1-3 distances within all simple rings are first dealt with, while keeping
  track of
  any atoms that are visited twice; these are the atoms that are part of
  multiple simple rings.
  Then all other 1-3 distance are set while treating 1-3 atoms that have a ring
  atom in
  between differently from those that have a non-ring atom in between.
 */
void set13Bounds(const ROMol &mol, DistGeom::BoundsMatPtr mmat,
                 ComputedData &accumData);

//! Set 1-4 distance bounds for atoms in a molecule
/*!
  These are computed using the range of allowed torsion angles. There are
  several
  special cases and the rest are computed using 0 and 180 deg as the min.
  and max. torsion angles. The special cases deal with ring systems, double
  bonds
  with cis-trans specifications, and a few special sub-structures
  \param mol          Molecule of interest
  \param mmat         Bounds matrix to which the bounds are written
  \param accumData    Used to store the data that have been calculated so far
                      about the molecule
  <b>Procedure</b>
  As in the case of 1-3 distances 1-4 distance that are part of simple rings are
  first dealt with. The remaining 1-4 cases are dealt with while paying
  attention
  to the special cases.
 */
void set14Bounds(const ROMol &mol, DistGeom::BoundsMatPtr mmat,
                 ComputedData &accumData, bool useMacrocycle14config = false);

//! Set 1-5 distance bounds for atoms in a molecule
/*!
  This is an optional call that recognizes a variety of special cases.
  \param mol          Molecule of interest
  \param mmat         Bounds matrix to which the bounds are written
  \param accumData    Used to store the data that have been calculated so far
                      about the molecule
*/
void set15Bounds(const ROMol &mol, DistGeom::BoundsMatPtr mmat,
                 ComputedData &accumData, double *distMatrix);

//! Set lower distance bounds based on VDW radii for atoms that are not covered
// by
//! other bounds (1-2, 1-3, 1-4, or 1-5)
/*!
  \param mol             Molecule of interest
  \param mmat            Bounds matrix to which the bounds are written
  \param useTopolScaling If true scale the sum of the vdW radii while setting
  lower bounds
                         so that a smaller value (0.7*(vdw1 + vdw2) ) is used
  for paths
                         that are less 5 bonds apart.
*/
void setLowerBoundVDW(const ROMol &mol, DistGeom::BoundsMatPtr mmat,
                      bool useTopolScaling = true);
}  // namespace DGeomHelpers
}  // namespace RDKit

namespace RDKit {
namespace DGeomHelpers {
void _checkAndSetBounds(unsigned int i, unsigned int j, double lb, double ub,
                        DistGeom::BoundsMatPtr mmat) {
  // get the existing bounds
  double clb = mmat->getLowerBound(i, j);
  double cub = mmat->getUpperBound(i, j);

  CHECK_INVARIANT(ub > lb, "upper bound not greater than lower bound");
  CHECK_INVARIANT(lb > DIST12_DELTA || clb > DIST12_DELTA, "bad lower bound");

  if (clb <= DIST12_DELTA) {
    mmat->setLowerBound(i, j, lb);
  } else {
    if ((lb < clb) && (lb > DIST12_DELTA)) {
      mmat->setLowerBound(i, j, lb);  // conservative bound setting
    }
  }

  if (cub >= MAX_UPPER) {  // FIX this
    mmat->setUpperBound(i, j, ub);
  } else {
    if ((ub > cub) && (ub < MAX_UPPER)) {
      mmat->setUpperBound(i, j, ub);
    }
  }
}

void set12Bounds(const ROMol &mol, DistGeom::BoundsMatPtr mmat,
                 ComputedData &accumData) {
  unsigned int npt = mmat->numRows();
  CHECK_INVARIANT(npt == mol.getNumAtoms(), "Wrong size metric matrix");
  CHECK_INVARIANT(accumData.bondLengths.size() >= mol.getNumBonds(),
                  "Wrong size accumData");
  auto [atomParams, foundAll] = UFF::getAtomTypes(mol);
  CHECK_INVARIANT(atomParams.size() == mol.getNumAtoms(),
                  "parameter vector size mismatch");

  boost::dynamic_bitset<> squishAtoms(mol.getNumAtoms());
  // find larger heteroatoms in conjugated 5 rings, because we need to add a bit
  // of extra flex for them
  for (const auto bond : mol.bonds()) {
    if (bond->getIsConjugated() &&
        (bond->getBeginAtom()->getAtomicNum() > 10 ||
         bond->getEndAtom()->getAtomicNum() > 10) &&
        mol.getRingInfo() && mol.getRingInfo()->isInitialized() &&
        mol.getRingInfo()->isBondInRingOfSize(bond->getIdx(), 5)) {
      squishAtoms.set(bond->getBeginAtomIdx());
      squishAtoms.set(bond->getEndAtomIdx());
    }
  }

  for (const auto bond : mol.bonds()) {
    auto begId = bond->getBeginAtomIdx();
    auto endId = bond->getEndAtomIdx();
    auto bOrder = bond->getBondTypeAsDouble();
    if (atomParams[begId] && atomParams[endId] && bOrder > 0) {
      auto bl = ForceFields::UFF::Utils::calcBondRestLength(
          bOrder, atomParams[begId], atomParams[endId]);

      double extraSquish = 0.0;
      if (squishAtoms[begId] || squishAtoms[endId]) {
        extraSquish = 0.2;  // empirical
      }

      accumData.bondLengths[bond->getIdx()] = bl;
      mmat->setUpperBound(begId, endId, bl + extraSquish + DIST12_DELTA);
      mmat->setLowerBound(begId, endId, bl - extraSquish - DIST12_DELTA);
    } else {
      // we don't have parameters for one of the atoms... so we're forced to
      // use very crude bounds:
      auto vw1 = PeriodicTable::getTable()->getRvdw(
          mol.getAtomWithIdx(begId)->getAtomicNum());
      auto vw2 = PeriodicTable::getTable()->getRvdw(
          mol.getAtomWithIdx(endId)->getAtomicNum());
      auto bl = (vw1 + vw2) / 2;
      accumData.bondLengths[bond->getIdx()] = bl;
      mmat->setUpperBound(begId, endId, 1.5 * bl);
      mmat->setLowerBound(begId, endId, .5 * bl);
    }
  }
}

void setLowerBoundVDW(const ROMol &mol, DistGeom::BoundsMatPtr mmat, bool,
                      double *dmat) {
  unsigned int npt = mmat->numRows();
  PRECONDITION(npt == mol.getNumAtoms(), "Wrong size metric matrix");

  for (unsigned int i = 1; i < npt; i++) {
    auto vw1 = PeriodicTable::getTable()->getRvdw(
        mol.getAtomWithIdx(i)->getAtomicNum());
    for (unsigned int j = 0; j < i; j++) {
      auto vw2 = PeriodicTable::getTable()->getRvdw(
          mol.getAtomWithIdx(j)->getAtomicNum());
      if (mmat->getLowerBound(i, j) < DIST12_DELTA) {
        // ok this is what we are going to do
        // - for atoms that are 4 or 5 bonds apart (15 or 16 distances), we
        // will scale
        //   the sum of the VDW radii so that the atoms can get closer
        //   For 15 we will use VDW_SCALE_15 and for 16 we will use 1 -
        //   0.5*VDW_SCALE_15
        // - for all other pairs of atoms more than 5 bonds apart we use the
        // sum of the VDW radii
        //    as the lower bound
        if (dmat[i * npt + j] == 4.0) {
          mmat->setLowerBound(i, j, VDW_SCALE_15 * (vw1 + vw2));
        } else if (dmat[i * npt + j] == 5.0) {
          mmat->setLowerBound(
              i, j, (VDW_SCALE_15 + 0.5 * (1.0 - VDW_SCALE_15)) * (vw1 + vw2));
        } else {
          mmat->setLowerBound(i, j, (vw1 + vw2));
        }
      }
    }
  }
}

namespace {
bool isLargerSP2Atom(const Atom *atom) {
  return atom->getAtomicNum() > 13 && atom->getHybridization() == Atom::SP2 &&
         atom->getOwningMol().getRingInfo()->numAtomRings(atom->getIdx());
}
}  // namespace
void _set13BoundsHelper(unsigned int aid1, unsigned int aid, unsigned int aid3,
                        double angle, const ComputedData &accumData,
                        DistGeom::BoundsMatPtr mmat, const ROMol &mol) {
  auto bid1 = mol.getBondBetweenAtoms(aid1, aid)->getIdx();
  auto bid2 = mol.getBondBetweenAtoms(aid, aid3)->getIdx();
  auto dl = RDGeom::compute13Dist(accumData.bondLengths[bid1],
                                  accumData.bondLengths[bid2], angle);
  auto distTol = DIST13_TOL;
  // Now increase the tolerance if we're outside of the first row of the
  // periodic table.
  if (isLargerSP2Atom(mol.getAtomWithIdx(aid1))) {
    distTol *= 2;
  }
  if (isLargerSP2Atom(mol.getAtomWithIdx(aid))) {
    distTol *= 2;
  }
  if (isLargerSP2Atom(mol.getAtomWithIdx(aid3))) {
    distTol *= 2;
  }
  auto du = dl + distTol;
  dl -= distTol;
  _checkAndSetBounds(aid1, aid3, dl, du, mmat);
}

void _setRingAngle(Atom::HybridizationType aHyb, unsigned int ringSize,
                   double &angle) {
  // NOTE: this assumes that all angles in a ring are equal. This is
  // certainly not always the case, particular in aromatic rings with
  // heteroatoms
  // like s1cncc1. This led to GitHub55, which was fixed elsewhere.

  if ((aHyb == Atom::SP2 && ringSize <= 8) || (ringSize == 3) ||
      (ringSize == 4)) {
    angle = M_PI * (1 - 2.0 / ringSize);
  } else if (aHyb == Atom::SP3) {
    if (ringSize == 5) {
      angle = 104 * M_PI / 180;
    } else {
      angle = 109.5 * M_PI / 180;
    }
  } else if (aHyb == Atom::SP3D) {
    angle = 105.0 * M_PI / 180;
  } else if (aHyb == Atom::SP3D2) {
    angle = 90.0 * M_PI / 180;
  } else {
    angle = 120 * M_PI / 180;
  }
}

auto lessVector = [](const auto &v1, const auto &v2) {
  return v1.size() < v2.size();
};

void set13Bounds(const ROMol &mol, DistGeom::BoundsMatPtr mmat,
                 ComputedData &accumData) {
  auto npt = mmat->numRows();
  CHECK_INVARIANT(npt == mol.getNumAtoms(), "Wrong size metric matrix");
  CHECK_INVARIANT(accumData.bondAngles->numRows() == mol.getNumBonds(),
                  "Wrong size bond angle matrix");
  CHECK_INVARIANT(accumData.bondAdj->numRows() == mol.getNumBonds(),
                  "Wrong size bond adjacency matrix");

  // Since most of the special cases arise out of ring system, we will do
  // the following here:
  // - Loop over all the rings and set the 13 distances between atoms in
  // these rings.
  //   While doing this keep track of the ring atoms that have already been
  //   used as the center atom.
  // - Set the 13 distance between atoms that have a ring atom in between;
  // these can be either non-ring atoms,
  //   or a ring atom and a non-ring atom, or ring atoms that belong to
  //   different simple rings
  // - finally set all other 13 distances
  const auto rinfo = mol.getRingInfo();
  CHECK_INVARIANT(rinfo, "");

  unsigned int aid2, aid1, aid3, bid1, bid2;
  double angle;

  auto atomRings = rinfo->atomRings();
  std::sort(atomRings.begin(), atomRings.end(), lessVector);
  // sort the rings based on the ring size
  INT_VECT visited(npt, 0);

  DOUBLE_VECT angleTaken(npt, 0.0);
  auto nb = mol.getNumBonds();
  BIT_SET donePaths(nb * nb);
  // first deal with all rings and atoms in them
  for (const auto &ringi : atomRings) {
    auto rSize = ringi.size();
    aid1 = ringi[rSize - 1];
    for (unsigned int i = 0; i < rSize; i++) {
      aid2 = ringi[i];
      if (i == rSize - 1) {
        aid3 = ringi[0];
      } else {
        aid3 = ringi[i + 1];
      }
      const auto b1 = mol.getBondBetweenAtoms(aid1, aid2);
      const auto b2 = mol.getBondBetweenAtoms(aid2, aid3);
      CHECK_INVARIANT(b1, "no bond found");
      CHECK_INVARIANT(b2, "no bond found");
      bid1 = b1->getIdx();
      bid2 = b2->getIdx();
      auto id1 = nb * bid1 + bid2;
      auto id2 = nb * bid2 + bid1;

      if ((!donePaths[id1]) && (!donePaths[id2])) {
        // this invar stuff is to deal with bridged systems (Issue 215). In
        // bridged
        // systems we may be covering the same 13 (ring) paths multiple
        // times and unnecessarily increasing the angleTaken at the central
        // atom.
        _setRingAngle(mol.getAtomWithIdx(aid2)->getHybridization(), rSize,
                      angle);
        _set13BoundsHelper(aid1, aid2, aid3, angle, accumData, mmat, mol);
        accumData.bondAngles->setVal(bid1, bid2, angle);
        accumData.bondAdj->setVal(bid1, bid2, aid2);
        visited[aid2] += 1;
        angleTaken[aid2] += angle;
        donePaths[id1] = 1;
        donePaths[id2] = 1;
        // donePaths.push_back(invar);
      }
      aid1 = aid2;
    }
  }

  // now deal with the remaining atoms
  for (aid2 = 0; aid2 < npt; aid2++) {
    const auto atom = mol.getAtomWithIdx(aid2);
    auto deg = atom->getDegree();
    auto n13 = deg * (deg - 1) / 2;
    if (n13 == static_cast<unsigned int>(visited[aid2])) {
      // we are done with this atom
      continue;
    }
    auto ahyb = atom->getHybridization();
    auto [beg1, end1] = mol.getAtomBonds(atom);
    if (visited[aid2] >= 1) {
      // deal with atoms that we already visited; i.e. ring atoms. Set 13
      // distances for one of following cases:
      //  1) Non-ring atoms that have a ring atom in-between
      //  2) Non-ring atom and a ring atom that have a ring atom in between
      //  3) Ring atoms that belong to different rings (that are part of a
      //  fused system

      while (beg1 != end1) {
        const auto bnd1 = mol[*beg1];
        bid1 = bnd1->getIdx();
        aid1 = bnd1->getOtherAtomIdx(aid2);
        auto [beg2, end2] = mol.getAtomBonds(atom);
        while (beg2 != beg1) {
          const auto bnd2 = mol[*beg2];
          bid2 = bnd2->getIdx();
          aid3 = bnd2->getOtherAtomIdx(aid2);
          if (accumData.bondAngles->getVal(bid1, bid2) < 0.0) {
            // if we haven't dealt with these two bonds before

            // if we have a sp2 atom things are planar - we simply divide
            // the remaining angle among the remaining 13 configurations
            // (and there should only be one)
            if (ahyb == Atom::SP2) {
              angle = (2 * M_PI - angleTaken[aid2]) / (n13 - visited[aid2]);
            } else if (ahyb == Atom::SP3) {
              // in the case of sp3 we will use the tetrahedral angle mostly
              // - but with some special cases
              angle = 109.5 * M_PI / 180;
              // we will special-case a little bit here for 3, 4 members
              // ring atoms that are sp3 hybridized beyond that the angle
              // reasonably close to the tetrahedral angle
              if (rinfo->isAtomInRingOfSize(aid2, 3)) {
                angle = 116.0 * M_PI / 180;
              } else if (rinfo->isAtomInRingOfSize(aid2, 4)) {
                angle = 112.0 * M_PI / 180;
              }
            } else if (Chirality::hasNonTetrahedralStereo(atom)) {
              angle = Chirality::getIdealAngleBetweenLigands(
                          atom, mol.getAtomWithIdx(aid1),
                          mol.getAtomWithIdx(aid3)) *
                      M_PI / 180;
            } else {
              // other options we will simply based things on the number of
              // substituent
              if (deg == 5) {
                angle = 105.0 * M_PI / 180;
              } else if (deg == 6) {
                angle = 135.0 * M_PI / 180;
              } else {
                angle = 120.0 * M_PI / 180;  // FIX: this default is probably
                                             // not the best we can do here
              }
            }
            _set13BoundsHelper(aid1, aid2, aid3, angle, accumData, mmat, mol);
            accumData.bondAngles->setVal(bid1, bid2, angle);
            accumData.bondAdj->setVal(bid1, bid2, aid2);
            angleTaken[aid2] += angle;
            visited[aid2] += 1;
          }
          ++beg2;
        }  // while loop over the second bond
        ++beg1;
      }  // while loop over the first bond
    } else if (visited[aid2] == 0) {
      // non-ring atoms - we will simply use angles based on hybridization
      while (beg1 != end1) {
        const auto bnd1 = mol[*beg1];
        bid1 = bnd1->getIdx();
        aid1 = bnd1->getOtherAtomIdx(aid2);
        auto [beg2, end2] = mol.getAtomBonds(atom);
        while (beg2 != beg1) {
          const auto bnd2 = mol[*beg2];
          bid2 = bnd2->getIdx();
          aid3 = bnd2->getOtherAtomIdx(aid2);
          if (Chirality::hasNonTetrahedralStereo(atom)) {
            angle =
                Chirality::getIdealAngleBetweenLigands(
                    atom, mol.getAtomWithIdx(aid1), mol.getAtomWithIdx(aid3)) *
                M_PI / 180;

          } else {
            if (ahyb == Atom::SP) {
              angle = M_PI;
            } else if (ahyb == Atom::SP2) {
              angle = 2 * M_PI / 3;
            } else if (ahyb == Atom::SP3) {
              angle = 109.5 * M_PI / 180;
            } else if (Chirality::hasNonTetrahedralStereo(atom)) {
              angle = Chirality::getIdealAngleBetweenLigands(
                          atom, mol.getAtomWithIdx(aid1),
                          mol.getAtomWithIdx(aid3)) *
                      M_PI / 180;
            } else if (ahyb == Atom::SP3D) {
              // FIX: this and the remaining two hybridization states below
              // should probably be special cased. These defaults below are
              // probably not the best we can do particularly when stereo
              // chemistry is know
              angle = 105.0 * M_PI / 180;
            } else if (ahyb == Atom::SP3D2) {
              angle = 135.0 * M_PI / 180;
            } else {
              angle = 120.0 * M_PI / 180;
            }
          }
          if (atom->getDegree() <= 4 ||
              (Chirality::hasNonTetrahedralStereo(atom) &&
               atom->hasProp(common_properties::_chiralPermutation))) {
            _set13BoundsHelper(aid1, aid2, aid3, angle, accumData, mmat, mol);
          } else {
            // just use 180 as the max angle and an arbitrary min angle
            auto dmax =
                accumData.bondLengths[bid1] + accumData.bondLengths[bid2];
            auto dl = 1.0;
            auto du = dmax * 1.2;
            _checkAndSetBounds(aid1, aid3, dl, du, mmat);
          }
          accumData.bondAngles->setVal(bid1, bid2, angle);
          accumData.bondAdj->setVal(bid1, bid2, aid2);
          angleTaken[aid2] += angle;
          visited[aid2] += 1;
          ++beg2;
        }  // while loop over second bond
        ++beg1;
      }  // while loop over first bond
    }  // done with non-ring atoms
  }  // done with all atoms
}  // done with 13 distance setting

Bond::BondStereo _getAtomStereo(const Bond *bnd, unsigned int aid1,
                                unsigned int aid4) {
  auto stype = bnd->getStereo();
  if (stype > Bond::STEREOANY && bnd->getStereoAtoms().size() >= 2) {
    const auto &stAtoms = bnd->getStereoAtoms();
    if ((static_cast<unsigned int>(stAtoms[0]) != aid1) ^
        (static_cast<unsigned int>(stAtoms[1]) != aid4)) {
      switch (stype) {
        case Bond::STEREOZ:
          stype = Bond::STEREOE;
          break;
        case Bond::STEREOE:
          stype = Bond::STEREOZ;
          break;
        case Bond::STEREOCIS:
          stype = Bond::STEREOTRANS;
          break;
        case Bond::STEREOTRANS:
          stype = Bond::STEREOCIS;
          break;
        default:
          break;
      }
    }
  }
  return stype;
}

void _setInRing14Bounds(const ROMol &mol, const Bond *bnd1, const Bond *bnd2,
                        const Bond *bnd3, ComputedData &accumData,
                        DistGeom::BoundsMatPtr mmat, double *dmat,
                        int ringSize) {
  PRECONDITION(bnd1, "");
  PRECONDITION(bnd2, "");
  PRECONDITION(bnd3, "");
  unsigned int bid1, bid2, bid3;
  bid1 = bnd1->getIdx();
  bid2 = bnd2->getIdx();
  bid3 = bnd3->getIdx();
  const Atom *atm2 = mol.getAtomWithIdx(accumData.bondAdj->getVal(bid1, bid2));
  PRECONDITION(atm2, "");
  Atom::HybridizationType ahyb2 = atm2->getHybridization();
  const Atom *atm3 = mol.getAtomWithIdx(accumData.bondAdj->getVal(bid2, bid3));
  PRECONDITION(atm3, "");
  Atom::HybridizationType ahyb3 = atm3->getHybridization();

  unsigned int aid1 = bnd1->getOtherAtomIdx(atm2->getIdx());
  unsigned int aid4 = bnd3->getOtherAtomIdx(atm3->getIdx());

  // check that this actually is a 1-4 contact:
  if (dmat[std::max(aid1, aid4) * mmat->numRows() + std::min(aid1, aid4)] <
      2.9) {
    // std::cerr<<"skip: "<<aid1<<"-"<<aid4<<" because
    // d="<<dmat[std::max(aid1,aid4)*mmat->numRows()+std::min(aid1,aid4)]<<std::endl;
    return;
  }

  double bl1 = accumData.bondLengths[bid1];
  double bl2 = accumData.bondLengths[bid2];
  double bl3 = accumData.bondLengths[bid3];

  double ba12 = accumData.bondAngles->getVal(bid1, bid2);
  double ba23 = accumData.bondAngles->getVal(bid2, bid3);

  CHECK_INVARIANT(ba12 > 0.0, "");
  CHECK_INVARIANT(ba23 > 0.0, "");
  double dl, du;
  unsigned int nb = mol.getNumBonds();
  // several special cases here
  Path14Configuration path14;
  path14.bid1 = bid1;
  path14.bid2 = bid2;
  path14.bid3 = bid3;
  Bond::BondStereo stype = _getAtomStereo(bnd2, aid1, aid4);
  bool preferCis = false;
  bool preferTrans = false;

  // we add a check for the ring size here because there's no reason to
  // assume cis bonds in bigger rings. This was part of github #1240:
  // failure to embed larger aromatic rings
  if (ringSize <= 8 && (ahyb2 == Atom::SP2) && (ahyb3 == Atom::SP2) &&
      (stype != Bond::STEREOE && stype != Bond::STEREOTRANS)) {
    // the ring check here was a big part of github #697
    if (mol.getRingInfo()->numBondRings(bid2) > 1) {
      if (mol.getRingInfo()->numBondRings(bid1) == 1 &&
          mol.getRingInfo()->numBondRings(bid3) == 1) {
        for (const auto &br : mol.getRingInfo()->bondRings()) {
          if (std::find(br.begin(), br.end(), bid1) != br.end()) {
            if (std::find(br.begin(), br.end(), bid3) != br.end()) {
              preferCis = true;
            }
            break;
          }
        }
      }
    } else {
      preferCis = true;
    }
  } else if (stype == Bond::STEREOZ || stype == Bond::STEREOCIS) {
    preferCis = true;
  } else if (stype == Bond::STEREOE || stype == Bond::STEREOTRANS) {
    preferTrans = true;
  }
  // std::cerr << "  torsion: " << aid1 << " " << aid4 << ": " << preferCis
  // << "
  // "
  //           << preferTrans << std::endl;
  if (preferCis) {
    dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23) - GEN_DIST_TOL;
    du = dl + 2 * GEN_DIST_TOL;
    path14.type = Path14Configuration::CIS;
    accumData.cisPaths.insert(static_cast<unsigned long>(bid1) * nb * nb +
                              bid2 * nb + bid3);
    accumData.cisPaths.insert(static_cast<unsigned long>(bid3) * nb * nb +
                              bid2 * nb + bid1);
  } else if (preferTrans) {
    dl = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23) - GEN_DIST_TOL;
    du = dl + 2 * GEN_DIST_TOL;
    path14.type = Path14Configuration::TRANS;
    accumData.transPaths.insert(static_cast<unsigned long>(bid1) * nb * nb +
                                bid2 * nb + bid3);
    accumData.transPaths.insert(static_cast<unsigned long>(bid3) * nb * nb +
                                bid2 * nb + bid1);

  } else {
    // basically we will assume 0 to 180 allowed
    dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
    du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
    if (du < dl) {
      std::swap(du, dl);
    }
    if (fabs(du - dl) < DIST12_DELTA) {
      dl -= GEN_DIST_TOL;
      du += GEN_DIST_TOL;
    }
    path14.type = Path14Configuration::OTHER;
  }

  // std::cerr << "7: " << aid1 << "-" << aid4 << std::endl;
  _checkAndSetBounds(aid1, aid4, dl, du, mmat);
  accumData.paths14.push_back(path14);
}

void _setTwoInSameRing14Bounds(const ROMol &mol, const Bond *bnd1,
                               const Bond *bnd2, const Bond *bnd3,
                               ComputedData &accumData,
                               DistGeom::BoundsMatPtr mmat, double *dmat) {
  PRECONDITION(bnd1, "");
  PRECONDITION(bnd2, "");
  PRECONDITION(bnd3, "");
  unsigned int bid1, bid2, bid3;
  bid1 = bnd1->getIdx();
  bid2 = bnd2->getIdx();
  bid3 = bnd3->getIdx();
  const Atom *atm2 = mol.getAtomWithIdx(accumData.bondAdj->getVal(bid1, bid2));
  PRECONDITION(atm2, "");
  const Atom *atm3 = mol.getAtomWithIdx(accumData.bondAdj->getVal(bid2, bid3));
  PRECONDITION(atm3, "");

  unsigned int aid1 = bnd1->getOtherAtomIdx(atm2->getIdx());
  unsigned int aid4 = bnd3->getOtherAtomIdx(atm3->getIdx());

  // check that this actually is a 1-4 contact:
  if (dmat[std::max(aid1, aid4) * mmat->numRows() + std::min(aid1, aid4)] <
      2.9) {
    // std::cerr<<"skip: "<<aid1<<"-"<<aid4<<" because
    // d="<<dmat[std::max(aid1,aid4)*mmat->numRows()+std::min(aid1,aid4)]<<std::endl;
    return;
  }

  // when we have fused rings, it can happen that this isn't actually a 1-4
  // contact,
  // (this was the cause of sf.net bug 2835784) check that now:
  if (mol.getBondBetweenAtoms(aid1, atm3->getIdx()) ||
      mol.getBondBetweenAtoms(aid4, atm2->getIdx())) {
    return;
  }

  Atom::HybridizationType ahyb3 = atm3->getHybridization();
  Atom::HybridizationType ahyb2 = atm2->getHybridization();

  double bl1 = accumData.bondLengths[bid1];
  double bl2 = accumData.bondLengths[bid2];
  double bl3 = accumData.bondLengths[bid3];

  double ba12 = accumData.bondAngles->getVal(bid1, bid2);
  double ba23 = accumData.bondAngles->getVal(bid2, bid3);
  CHECK_INVARIANT(ba12 > 0.0, "");
  CHECK_INVARIANT(ba23 > 0.0, "");
  double dl, du;
  Path14Configuration path14;
  unsigned int nb = mol.getNumBonds();

  path14.bid1 = bid1;
  path14.bid2 = bid2;
  path14.bid3 = bid3;
  if ((ahyb2 == Atom::SP2) && (ahyb3 == Atom::SP2)) {  // FIX: check for trans
    // here we will assume 180 degrees: basically flat ring with an external
    // substituent
    dl = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
    du = dl;
    dl -= GEN_DIST_TOL;
    du += GEN_DIST_TOL;
    path14.type = Path14Configuration::TRANS;
    accumData.transPaths.insert(static_cast<unsigned long>(bid1) * nb * nb +
                                bid2 * nb + bid3);
    accumData.transPaths.insert(static_cast<unsigned long>(bid3) * nb * nb +
                                bid2 * nb + bid1);

  } else {
    // here we will assume anything is possible
    dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
    du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);

    // in highly-strained situations these can get mixed up:
    if (du < dl) {
      double tmpD = dl;
      dl = du;
      du = tmpD;
    }
    if (fabs(du - dl) < DIST12_DELTA) {
      dl -= GEN_DIST_TOL;
      du += GEN_DIST_TOL;
    }
    path14.type = Path14Configuration::OTHER;
  }
  // std::cerr << "1: " << aid1 << "-" << aid4 << ": " << dl << " -> " << du
  //           << std::endl;
  _checkAndSetBounds(aid1, aid4, dl, du, mmat);
  accumData.paths14.push_back(path14);
}

void _setTwoInDiffRing14Bounds(const ROMol &mol, const Bond *bnd1,
                               const Bond *bnd2, const Bond *bnd3,
                               ComputedData &accumData,
                               DistGeom::BoundsMatPtr mmat, double *dmat) {
  // this turns out to be very similar to all bonds in the same ring
  // situation.
  // There is probably some fine tuning that can be done when the atoms a2
  // and a3 are not sp2 hybridized, but we will not worry about that now;
  // simple use 0-180 deg for non-sp2 cases.
  _setInRing14Bounds(mol, bnd1, bnd2, bnd3, accumData, mmat, dmat, 0);
}

void _setShareRingBond14Bounds(const ROMol &mol, const Bond *bnd1,
                               const Bond *bnd2, const Bond *bnd3,
                               ComputedData &accumData,
                               DistGeom::BoundsMatPtr mmat, double *dmat) {
  // once this turns out to be similar to bonds in the same ring
  _setInRing14Bounds(mol, bnd1, bnd2, bnd3, accumData, mmat, dmat, 0);
}

bool _checkH2NX3H1OX2(const Atom *atm) {
  if ((atm->getAtomicNum() == 6) && (atm->getTotalNumHs(true) == 2)) {
    // CH2
    return true;
  } else if ((atm->getAtomicNum() == 8) && (atm->getTotalNumHs(true) == 0)) {
    // OX2
    return true;
  } else if ((atm->getAtomicNum() == 7) && (atm->getDegree() == 3) &&
             (atm->getTotalNumHs(true) == 1)) {
    // FIX: assuming hydrogen is not in the graph
    // this is NX3H1 situation
    return true;
  }
  return false;
}

bool _checkNhChChNh(const Atom *atm1, const Atom *atm2, const Atom *atm3,
                    const Atom *atm4) {
  // checking for [!#1]~$ch!@$ch~[!#1], where ch = [CH2,NX3H1,OX2] situation
  if ((atm1->getAtomicNum() != 1) && (atm4->getAtomicNum() != 1)) {
    // end atom not hydrogens
    if ((_checkH2NX3H1OX2(atm2)) && (_checkH2NX3H1OX2(atm3))) {
      return true;
    }
  }
  return false;
}

// here we look for something like this:
// It's an amide or ester:
//
//        4    <- 4 is the O
//        |    <- That's the double bond
//    1   3
//     \ / \                                         T.S.I.Left Blank
//      2   5  <- 2 is an oxygen/nitrogen
bool _checkAmideEster14(const Bond *bnd1, const Bond *bnd3, const Atom *,
                        const Atom *atm2, const Atom *atm3, const Atom *atm4) {
  unsigned int a2Num = atm2->getAtomicNum();
  unsigned int a3Num = atm3->getAtomicNum();
  unsigned int a4Num = atm4->getAtomicNum();
  // std::cerr << " -> " << atm1->getIdx() << "-" << atm2->getIdx() << "-"
  //           << atm3->getIdx() << "-" << atm4->getIdx()
  //           << " bonds: " << bnd1->getIdx() << "," << bnd3->getIdx()
  //           << std::endl;
  // std::cerr << "   " << a1Num << " " << a3Num << " " <<
  // bnd3->getBondType()
  //           << " " << a4Num << " " << bnd1->getBondType() << " " << a2Num
  //           << " "
  //           << atm2->getTotalNumHs(true) << std::endl;
  if (a3Num == 6 && bnd3->getBondType() == Bond::DOUBLE &&
      (a4Num == 8 || a4Num == 7) && bnd1->getBondType() == Bond::SINGLE &&
      (a2Num == 8 || (a2Num == 7 && atm2->getTotalNumHs(true) == 1))) {
    // std::cerr << " yes!" << std::endl;
    return true;
  }
  // std::cerr << " no!" << std::endl;
  return false;
}

// checking for amide/ester when all three bonds are
// part of the macrocycle ring
// here we look for something like this:
// It's an amide or ester:
//
//        5    <- 5 is the O
//        |    <- That's the double bond
//    1   3
//     \ / \                                         T.S.I.Left Blank
//      2   4  <- 2 is an oxygen/nitrogen
bool _checkMacrocycleAllInSameRingAmideEster14(const ROMol &mol, const Bond *,
                                               const Bond *, const Atom *atm1,
                                               const Atom *atm2,
                                               const Atom *atm3,
                                               const Atom *atm4) {
  //   This is a re-write of `_checkAmideEster14` with more explicit logic
  //   on the checks It is interesting that we find with this function we
  //   get better macrocycle sampling than `_checkAmideEster14`
  unsigned int a2Num = atm2->getAtomicNum();
  unsigned int a3Num = atm3->getAtomicNum();

  if (a3Num != 6) {
    return false;
  }

  if (a2Num == 7 || a2Num == 8) {
    if (mol.getAtomDegree(atm2) == 3 && mol.getAtomDegree(atm3) == 3) {
      for (auto nbrIdx :
           boost::make_iterator_range(mol.getAtomNeighbors(atm2))) {
        if (nbrIdx != atm1->getIdx() && nbrIdx != atm3->getIdx()) {
          const auto &res = mol.getAtomWithIdx(nbrIdx);
          const auto &resbnd = mol.getBondBetweenAtoms(atm2->getIdx(), nbrIdx);
          if ((res->getAtomicNum() != 6 &&
               res->getAtomicNum() != 1) ||  // check is (methylated)amide
              resbnd->getBondType() != Bond::SINGLE) {
            return false;
          }
          break;
        }
      }

      for (auto nbrIdx :
           boost::make_iterator_range(mol.getAtomNeighbors(atm3))) {
        if (nbrIdx != atm2->getIdx() && nbrIdx != atm4->getIdx()) {
          const auto &res = mol.getAtomWithIdx(nbrIdx);
          const auto &resbnd = mol.getBondBetweenAtoms(atm3->getIdx(), nbrIdx);
          if (res->getAtomicNum() != 8 ||  // check for the carbonyl oxygen
              resbnd->getBondType() != Bond::DOUBLE) {
            return false;
          }
          break;
        }
      }

      return true;
    }
  }
  return false;
}

bool _isCarbonyl(const ROMol &mol, const Atom *at) {
  PRECONDITION(at, "bad atom");
  if (at->getAtomicNum() == 6 && at->getDegree() > 2) {
    for (const auto nbr : mol.atomNeighbors(at)) {
      unsigned int atNum = nbr->getAtomicNum();
      if ((atNum == 8 || atNum == 7) &&
          mol.getBondBetweenAtoms(at->getIdx(), nbr->getIdx())->getBondType() ==
              Bond::DOUBLE) {
        return true;
      }
    }
  }
  return false;
}

bool _checkAmideEster15(const ROMol &mol, const Bond *bnd1, const Bond *bnd3,
                        const Atom *, const Atom *atm2, const Atom *atm3,
                        const Atom *) {
  unsigned int a2Num = atm2->getAtomicNum();
  if ((a2Num == 8) || ((a2Num == 7) && (atm2->getTotalNumHs(true) == 1))) {
    if ((bnd1->getBondType() == Bond::SINGLE)) {
      if ((atm3->getAtomicNum() == 6) &&
          (bnd3->getBondType() == Bond::SINGLE) && _isCarbonyl(mol, atm3)) {
        return true;
      }
    }
  }
  return false;
}

void _setChain14Bounds(const ROMol &mol, const Bond *bnd1, const Bond *bnd2,
                       const Bond *bnd3, ComputedData &accumData,
                       DistGeom::BoundsMatPtr mmat, double *,
                       bool forceTransAmides) {
  PRECONDITION(bnd1, "");
  PRECONDITION(bnd2, "");
  PRECONDITION(bnd3, "");
  unsigned int bid1, bid2, bid3;
  bid1 = bnd1->getIdx();
  bid2 = bnd2->getIdx();
  bid3 = bnd3->getIdx();
  const Atom *atm2 = mol.getAtomWithIdx(accumData.bondAdj->getVal(bid1, bid2));
  PRECONDITION(atm2, "");
  const Atom *atm3 = mol.getAtomWithIdx(accumData.bondAdj->getVal(bid2, bid3));
  PRECONDITION(atm3, "");

  unsigned int aid1 = bnd1->getOtherAtomIdx(atm2->getIdx());
  unsigned int aid4 = bnd3->getOtherAtomIdx(atm3->getIdx());
  const Atom *atm1 = mol.getAtomWithIdx(aid1);
  const Atom *atm4 = mol.getAtomWithIdx(aid4);

  double bl1 = accumData.bondLengths[bid1];
  double bl2 = accumData.bondLengths[bid2];
  double bl3 = accumData.bondLengths[bid3];

  double ba12 = accumData.bondAngles->getVal(bid1, bid2);
  double ba23 = accumData.bondAngles->getVal(bid2, bid3);
  CHECK_INVARIANT(ba12 > 0.0, "");
  CHECK_INVARIANT(ba23 > 0.0, "");
  bool setTheBound = true;
  double dl = 0.0, du = 0.0;

  // if the middle bond is double
  Path14Configuration path14;
  path14.bid1 = bid1;
  path14.bid2 = bid2;
  path14.bid3 = bid3;
  unsigned int nb = mol.getNumBonds();
  switch (bnd2->getBondType()) {
    case Bond::DOUBLE:
      // if any of the other bonds are double - the torsion angle is zero
      // this is CC=C=C situation
      if ((bnd1->getBondType() == Bond::DOUBLE) ||
          (bnd3->getBondType() == Bond::DOUBLE)) {
        dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23) - GEN_DIST_TOL;
        du = dl + 2 * GEN_DIST_TOL;
        path14.type = Path14Configuration::CIS;
        accumData.cisPaths.insert(static_cast<unsigned long>(bid1) * nb * nb +
                                  bid2 * nb + bid3);
        accumData.cisPaths.insert(static_cast<unsigned long>(bid3) * nb * nb +
                                  bid2 * nb + bid1);
        // BOOST_LOG(rdDebugLog) << "Special 5 " << aid1 << " " << aid4 <<
        // "\n";
      } else if (bnd2->getStereo() > Bond::STEREOANY) {
        Bond::BondStereo stype = _getAtomStereo(bnd2, aid1, aid4);
        if (stype == Bond::STEREOZ || stype == Bond::STEREOCIS) {
          dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23) -
               GEN_DIST_TOL;
          du = dl + 2 * GEN_DIST_TOL;
          path14.type = Path14Configuration::CIS;
          // BOOST_LOG(rdDebugLog) << "Special 6 " <<  aid1 << " " << aid4
          // <<
          // "\n";
          accumData.cisPaths.insert(static_cast<unsigned long>(bid1) * nb * nb +
                                    bid2 * nb + bid3);
          accumData.cisPaths.insert(static_cast<unsigned long>(bid3) * nb * nb +
                                    bid2 * nb + bid1);
        } else {
          // BOOST_LOG(rdDebugLog) << "Special 7 " << aid1 << " " << aid4 <<
          // "\n";
          du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
          dl = du;
          dl -= GEN_DIST_TOL;
          du += GEN_DIST_TOL;
          path14.type = Path14Configuration::TRANS;
          accumData.transPaths.insert(
              static_cast<unsigned long>(bid1) * nb * nb + bid2 * nb + bid3);
          accumData.transPaths.insert(
              static_cast<unsigned long>(bid3) * nb * nb + bid2 * nb + bid1);
        }
      } else {
        // double bond with no stereo setting can be 0 or 180
        dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
        du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
        if (fabs(du - dl) < DIST12_DELTA) {
          dl -= GEN_DIST_TOL;
          du += GEN_DIST_TOL;
        }
        path14.type = Path14Configuration::OTHER;
      }
      break;
    case Bond::SINGLE:
      if ((atm2->getAtomicNum() == 16) && (atm3->getAtomicNum() == 16)) {
        // this is *S-S* situation
        // FIX: this cannot be right is sulfur has more than two coordinated
        // the torsion angle is 90 deg
        dl = RDGeom::compute14Dist3D(bl1, bl2, bl3, ba12, ba23, M_PI / 2) -
             GEN_DIST_TOL;
        du = dl + 2 * GEN_DIST_TOL;
        path14.type = Path14Configuration::OTHER;
        // BOOST_LOG(rdDebugLog) << "Special 9 " << aid1 << " " << aid4 <<
        // "\n";
      } else if ((_checkAmideEster14(bnd1, bnd3, atm1, atm2, atm3, atm4)) ||
                 (_checkAmideEster14(bnd3, bnd1, atm4, atm3, atm2, atm1))) {
        // It's an amide or ester:
        //
        //        4    <- 4 is the O
        //        |    <- That's the double bond
        //    1   3
        //     \ / \                                         T.S.I.Left Blank
        //      2   5  <- 2 is an oxygen/nitrogen
        //
        // Here we set the distance between atoms 1 and 4,
        //  we'll handle atoms 1 and 5 below.

        // fix for issue 251 - we were marking this as a cis configuration
        // earlier
        // -------------------------------------------------------
        // Issue284:
        //   As this code originally stood, we forced amide bonds to be trans.
        //   This is convenient a lot of the time for generating nice-looking
        //   structures, but is unfortunately totally bogus.  So here we'll
        //   allow the distance to roam from cis to trans and hope that the
        //   force field planarizes things later.
        //
        //   What we'd really like to be able to do is specify multiple possible
        //   ranges for the distances, but a single bounds matrix doesn't
        //   support this kind of fanciness.
        //
        if (forceTransAmides) {
          if ((atm1->getAtomicNum() == 1 && atm2->getAtomicNum() == 7 &&
               atm2->getDegree() == 3 && atm2->getTotalNumHs(true) == 1) ||
              (atm4->getAtomicNum() == 1 && atm3->getAtomicNum() == 7 &&
               atm3->getDegree() == 3 && atm3->getTotalNumHs(true) == 1)) {
            // secondary amide, this is the H, it should be trans to the O
            dl = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
            path14.type = Path14Configuration::TRANS;
            accumData.transPaths.insert(
                static_cast<unsigned long>(bid1) * nb * nb + bid2 * nb + bid3);
            accumData.transPaths.insert(
                static_cast<unsigned long>(bid3) * nb * nb + bid2 * nb + bid1);
          } else {
            dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
            path14.type = Path14Configuration::CIS;
            accumData.cisPaths.insert(
                static_cast<unsigned long>(bid1) * nb * nb + bid2 * nb + bid3);
            accumData.cisPaths.insert(
                static_cast<unsigned long>(bid3) * nb * nb + bid2 * nb + bid1);
          }
          du = dl;
          dl -= GEN_DIST_TOL;
          du += GEN_DIST_TOL;
        } else {
          dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
          du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
          path14.type = Path14Configuration::OTHER;
        }
      } else if ((_checkAmideEster15(mol, bnd1, bnd3, atm1, atm2, atm3,
                                     atm4)) ||
                 (_checkAmideEster15(mol, bnd3, bnd1, atm4, atm3, atm2,
                                     atm1))) {
        // it's an amide or ester.
        //
        //        4    <- 4 is the O
        //        |    <- That's the double bond
        //    1   3
        //     \ / \                                          T.S.I.Left Blank
        //      2   5  <- 2 is oxygen or nitrogen
        //
        // we already set the 1-4 contact above, here we are doing 1-5

        // If we're going to have a hope of getting good geometries
        // out of here we need to set some reasonably smart bounds between 1
        // and 5 (ref Issue355):

        if (forceTransAmides) {
          if ((atm1->getAtomicNum() == 1 && atm2->getAtomicNum() == 7 &&
               atm2->getDegree() == 3 && atm2->getTotalNumHs(true) == 1) ||
              (atm4->getAtomicNum() == 1 && atm3->getAtomicNum() == 7 &&
               atm3->getDegree() == 3 && atm3->getTotalNumHs(true) == 1)) {
            // secondary amide, this is the H, it's cis to atom 5
            dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
            path14.type = Path14Configuration::CIS;
            accumData.cisPaths.insert(
                static_cast<unsigned long>(bid1) * nb * nb + bid2 * nb + bid3);
            accumData.cisPaths.insert(
                static_cast<unsigned long>(bid3) * nb * nb + bid2 * nb + bid1);
          } else {
            dl = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
            path14.type = Path14Configuration::TRANS;
            accumData.transPaths.insert(
                static_cast<unsigned long>(bid1) * nb * nb + bid2 * nb + bid3);
            accumData.transPaths.insert(
                static_cast<unsigned long>(bid3) * nb * nb + bid2 * nb + bid1);
          }
          du = dl;
          dl -= GEN_DIST_TOL;
          du += GEN_DIST_TOL;
        } else {
          dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
          du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
          path14.type = Path14Configuration::OTHER;
        }
      } else {
        dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
        du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
        path14.type = Path14Configuration::OTHER;
      }
      break;
    default:
      dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
      du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);

      path14.type = Path14Configuration::OTHER;
  }
  if (setTheBound) {
    if (fabs(du - dl) < DIST12_DELTA) {
      dl -= GEN_DIST_TOL;
      du += GEN_DIST_TOL;
    }
    _checkAndSetBounds(aid1, aid4, dl, du, mmat);
    accumData.paths14.push_back(path14);
  }
}

void _record14Path(const ROMol &mol, unsigned int bid1, unsigned int bid2,
                   unsigned int bid3, ComputedData &accumData) {
  const Atom *atm2 = mol.getAtomWithIdx(accumData.bondAdj->getVal(bid1, bid2));
  PRECONDITION(atm2, "");
  Atom::HybridizationType ahyb2 = atm2->getHybridization();
  const Atom *atm3 = mol.getAtomWithIdx(accumData.bondAdj->getVal(bid2, bid3));
  PRECONDITION(atm3, "");
  Atom::HybridizationType ahyb3 = atm3->getHybridization();
  unsigned int nb = mol.getNumBonds();
  Path14Configuration path14;
  path14.bid1 = bid1;
  path14.bid2 = bid2;
  path14.bid3 = bid3;
  if ((ahyb2 == Atom::SP2) && (ahyb3 == Atom::SP2)) {  // FIX: check for trans
    path14.type = Path14Configuration::CIS;
    accumData.cisPaths.insert(static_cast<unsigned long>(bid1) * nb * nb +
                              bid2 * nb + bid3);
    accumData.cisPaths.insert(static_cast<unsigned long>(bid3) * nb * nb +
                              bid2 * nb + bid1);
  } else {
    path14.type = Path14Configuration::OTHER;
  }
  accumData.paths14.push_back(path14);
}

// this is adapted from `_checkAmideEster14`, with only changing
// (a2Num == 7 && atm2->getTotalNumHs() == 1) into
// (a2Num == 7).
// This is necessary as the original function does not detect attached
// hydrogen even when is present (possibly due to explict/implicit H-count?),
// a new function is used (currently only for macrocycle treatment with ETKDGv3)
// in order to not break backward compatibility (also allow recognising
// methylated amide) here we look for something like this: It's an amide or
// ester:
//
//        4    <- 4 is the O
//        |    <- That's the double bond
//    1   3
//     \ / \                                         T.S.I.Left Blank
//      2   5  <- 2 is an oxygen/nitrogen
bool _checkMacrocycleTwoInSameRingAmideEster14(
    const Bond *bnd1, const Bond *bnd3, const Atom *atm1, const Atom *atm2,
    const Atom *atm3, const Atom *atm4) {
  unsigned int a1Num = atm1->getAtomicNum();
  unsigned int a2Num = atm2->getAtomicNum();
  unsigned int a3Num = atm3->getAtomicNum();
  unsigned int a4Num = atm4->getAtomicNum();

  return a1Num != 1 && a3Num == 6 && bnd3->getBondType() == Bond::DOUBLE &&
         (a4Num == 8 || a4Num == 7) && bnd1->getBondType() == Bond::SINGLE &&
         (a2Num == 8 || a2Num == 7);
}

void _setMacrocycleTwoInSameRing14Bounds(const ROMol &mol, const Bond *bnd1,
                                         const Bond *bnd2, const Bond *bnd3,
                                         ComputedData &accumData,
                                         DistGeom::BoundsMatPtr mmat,
                                         double *dmat) {
  PRECONDITION(bnd1, "");
  PRECONDITION(bnd2, "");
  PRECONDITION(bnd3, "");
  unsigned int bid1, bid2, bid3;
  bid1 = bnd1->getIdx();
  bid2 = bnd2->getIdx();
  bid3 = bnd3->getIdx();
  const Atom *atm2 = mol.getAtomWithIdx(accumData.bondAdj->getVal(bid1, bid2));
  PRECONDITION(atm2, "");
  const Atom *atm3 = mol.getAtomWithIdx(accumData.bondAdj->getVal(bid2, bid3));
  PRECONDITION(atm3, "");

  unsigned int aid1 = bnd1->getOtherAtomIdx(atm2->getIdx());
  unsigned int aid4 = bnd3->getOtherAtomIdx(atm3->getIdx());
  const Atom *atm1 = mol.getAtomWithIdx(aid1);
  const Atom *atm4 = mol.getAtomWithIdx(aid4);

  // check that this actually is a 1-4 contact:
  if (dmat[std::max(aid1, aid4) * mmat->numRows() + std::min(aid1, aid4)] <
      2.9) {
    // std::cerr<<"skip: "<<aid1<<"-"<<aid4<<" because
    // d="<<dmat[std::max(aid1,aid4)*mmat->numRows()+std::min(aid1,aid4)]<<std::endl;
    return;
  }

  // when we have fused rings, it can happen that this isn't actually a 1-4
  // contact,
  // (this was the cause of sf.net bug 2835784) check that now:
  if (mol.getBondBetweenAtoms(aid1, atm3->getIdx()) ||
      mol.getBondBetweenAtoms(aid4, atm2->getIdx())) {
    return;
  }

  double bl1 = accumData.bondLengths[bid1];
  double bl2 = accumData.bondLengths[bid2];
  double bl3 = accumData.bondLengths[bid3];

  double ba12 = accumData.bondAngles->getVal(bid1, bid2);
  double ba23 = accumData.bondAngles->getVal(bid2, bid3);
  CHECK_INVARIANT(ba12 > 0.0, "");
  CHECK_INVARIANT(ba23 > 0.0, "");
  double dl, du;
  Path14Configuration path14;
  unsigned int nb = mol.getNumBonds();

  path14.bid1 = bid1;
  path14.bid2 = bid2;
  path14.bid3 = bid3;
  if ((_checkMacrocycleTwoInSameRingAmideEster14(bnd1, bnd3, atm1, atm2, atm3,
                                                 atm4)) ||
      (_checkMacrocycleTwoInSameRingAmideEster14(bnd3, bnd1, atm4, atm3, atm2,
                                                 atm1))) {
    dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
    path14.type = Path14Configuration::CIS;
    accumData.cisPaths.insert(static_cast<unsigned long>(bid1) * nb * nb +
                              bid2 * nb + bid3);
    accumData.cisPaths.insert(static_cast<unsigned long>(bid3) * nb * nb +
                              bid2 * nb + bid1);
    du = dl;
    dl -= GEN_DIST_TOL;
    du += GEN_DIST_TOL;
  } else {
    // here we will assume anything is possible
    dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
    du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);

    // in highly-strained situations these can get mixed up:
    if (du < dl) {
      double tmpD = dl;
      dl = du;
      du = tmpD;
    }
    if (fabs(du - dl) < DIST12_DELTA) {
      dl -= GEN_DIST_TOL;
      du += GEN_DIST_TOL;
    }
    path14.type = Path14Configuration::OTHER;
  }
  // std::cerr << "1: " << aid1 << "-" << aid4 << ": " << dl << " -> " << du
  //           << std::endl;
  _checkAndSetBounds(aid1, aid4, dl, du, mmat);
  accumData.paths14.push_back(path14);
}

void _setMacrocycleAllInSameRing14Bounds(const ROMol &mol, const Bond *bnd1,
                                         const Bond *bnd2, const Bond *bnd3,
                                         ComputedData &accumData,
                                         DistGeom::BoundsMatPtr mmat,
                                         double *) {
  // This is adapted from `_setChain14Bounds`, with changes on how trans amide
  // is handled
  PRECONDITION(bnd1, "");
  PRECONDITION(bnd2, "");
  PRECONDITION(bnd3, "");
  unsigned int bid1, bid2, bid3;
  bid1 = bnd1->getIdx();
  bid2 = bnd2->getIdx();
  bid3 = bnd3->getIdx();
  const Atom *atm2 = mol.getAtomWithIdx(accumData.bondAdj->getVal(bid1, bid2));
  PRECONDITION(atm2, "");
  const Atom *atm3 = mol.getAtomWithIdx(accumData.bondAdj->getVal(bid2, bid3));
  PRECONDITION(atm3, "");

  unsigned int aid1 = bnd1->getOtherAtomIdx(atm2->getIdx());
  unsigned int aid4 = bnd3->getOtherAtomIdx(atm3->getIdx());

  const Atom *atm1 = mol.getAtomWithIdx(aid1);
  const Atom *atm4 = mol.getAtomWithIdx(aid4);

  double bl1 = accumData.bondLengths[bid1];
  double bl2 = accumData.bondLengths[bid2];
  double bl3 = accumData.bondLengths[bid3];

  double ba12 = accumData.bondAngles->getVal(bid1, bid2);
  double ba23 = accumData.bondAngles->getVal(bid2, bid3);
  CHECK_INVARIANT(ba12 > 0.0, "");
  CHECK_INVARIANT(ba23 > 0.0, "");
  bool setTheBound = true;
  double dl = 0.0, du = 0.0;

  // if the middle bond is double
  Path14Configuration path14;
  path14.bid1 = bid1;
  path14.bid2 = bid2;
  path14.bid3 = bid3;
  unsigned int nb = mol.getNumBonds();
  switch (bnd2->getBondType()) {
    case Bond::DOUBLE:
      // if any of the other bonds are double - the torsion angle is zero
      // this is CC=C=C situation
      if ((bnd1->getBondType() == Bond::DOUBLE) ||
          (bnd3->getBondType() == Bond::DOUBLE)) {
        dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23) - GEN_DIST_TOL;
        du = dl + 2 * GEN_DIST_TOL;
        path14.type = Path14Configuration::CIS;
        accumData.cisPaths.insert(static_cast<unsigned long>(bid1) * nb * nb +
                                  bid2 * nb + bid3);
        accumData.cisPaths.insert(static_cast<unsigned long>(bid3) * nb * nb +
                                  bid2 * nb + bid1);
        // BOOST_LOG(rdDebugLog) << "Special 5 " << aid1 << " " << aid4 <<
        // "\n";
      } else if (bnd2->getStereo() > Bond::STEREOANY) {
        Bond::BondStereo stype = _getAtomStereo(bnd2, aid1, aid4);
        if (stype == Bond::STEREOZ || stype == Bond::STEREOCIS) {
          dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23) -
               GEN_DIST_TOL;
          du = dl + 2 * GEN_DIST_TOL;
          path14.type = Path14Configuration::CIS;
          // BOOST_LOG(rdDebugLog) << "Special 6 " <<  aid1 << " " << aid4 <<
          // "\n";
          accumData.cisPaths.insert(static_cast<unsigned long>(bid1) * nb * nb +
                                    bid2 * nb + bid3);
          accumData.cisPaths.insert(static_cast<unsigned long>(bid3) * nb * nb +
                                    bid2 * nb + bid1);
        } else {
          // BOOST_LOG(rdDebugLog) << "Special 7 " << aid1 << " " << aid4 <<
          // "\n";
          du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
          dl = du;
          dl -= GEN_DIST_TOL;
          du += GEN_DIST_TOL;
          path14.type = Path14Configuration::TRANS;
          accumData.transPaths.insert(
              static_cast<unsigned long>(bid1) * nb * nb + bid2 * nb + bid3);
          accumData.transPaths.insert(
              static_cast<unsigned long>(bid3) * nb * nb + bid2 * nb + bid1);
        }
      } else {
        // double bond with no stereo setting can be 0 or 180
        dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
        du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
        if (fabs(du - dl) < DIST12_DELTA) {
          dl -= GEN_DIST_TOL;
          du += GEN_DIST_TOL;
        }
        path14.type = Path14Configuration::OTHER;
      }
      break;
    case Bond::SINGLE:
      if ((atm2->getAtomicNum() == 16) && (atm3->getAtomicNum() == 16)) {
        // this is *S-S* situation
        // FIX: this cannot be right is sulfur has more than two coordinated
        // the torsion angle is 90 deg
        dl = RDGeom::compute14Dist3D(bl1, bl2, bl3, ba12, ba23, M_PI / 2) -
             GEN_DIST_TOL;
        du = dl + 2 * GEN_DIST_TOL;
        path14.type = Path14Configuration::OTHER;
        // BOOST_LOG(rdDebugLog) << "Special 9 " << aid1 << " " << aid4 <<
        // "\n";
      } else if ((_checkMacrocycleAllInSameRingAmideEster14(
                     mol, bnd1, bnd3, atm1, atm2, atm3, atm4)) ||
                 (_checkMacrocycleAllInSameRingAmideEster14(
                     mol, bnd3, bnd1, atm4, atm3, atm2, atm1))) {
        dl =
            RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23) +
            0.1;  // we saw that the currently defined max distance for trans is
                  // still a bit too short, thus we add an additional 0.1, which
                  // is the max that works without triangular smoothing error
        path14.type = Path14Configuration::TRANS;
        accumData.transPaths.insert(static_cast<unsigned long>(bid1) * nb * nb +
                                    bid2 * nb + bid3);
        accumData.transPaths.insert(static_cast<unsigned long>(bid3) * nb * nb +
                                    bid2 * nb + bid1);

        du = dl;
        dl -= GEN_DIST_TOL;
        du += GEN_DIST_TOL;

        // BOOST_LOG(rdDebugLog) << "  amide: " << aid1 << " " << aid4 << ": "
        // << dl << "->" << du << "\n";
      } else if ((_checkAmideEster15(mol, bnd1, bnd3, atm1, atm2, atm3,
                                     atm4)) ||
                 (_checkAmideEster15(mol, bnd3, bnd1, atm4, atm3, atm2,
                                     atm1))) {
#ifdef FORCE_TRANS_AMIDES
        // amide is trans, we're cis:
        dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
        path14.type = Path14Configuration::CIS;
        accumData.cisPaths.insert(static_cast<unsigned long>(bid1) * nb * nb +
                                  bid2 * nb + bid3);
        accumData.cisPaths.insert(static_cast<unsigned long>(bid3) * nb * nb +
                                  bid2 * nb + bid1);
#else
        // amide is cis, we're trans:
        if (atm2->getAtomicNum() == 7 && atm2->getDegree() == 3 &&
            atm1->getAtomicNum() == 1 && atm2->getTotalNumHs(true) == 1) {
          // secondary amide, this is the H
          setTheBound = false;
        } else {
          dl = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
          path14.type = Path14Configuration::TRANS;
          accumData.transPaths.insert(
              static_cast<unsigned long>(bid1) * nb * nb + bid2 * nb + bid3);
          accumData.transPaths.insert(
              static_cast<unsigned long>(bid3) * nb * nb + bid2 * nb + bid1);
        }
#endif
        du = dl;
        dl -= GEN_DIST_TOL;
        du += GEN_DIST_TOL;
        // BOOST_LOG(rdDebugLog) << "    amide neighbor: " << aid1 << " " <<
        // aid4 << ": " << dl << "->" << du << "\n";
      } else {
        dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
        du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);
        path14.type = Path14Configuration::OTHER;
      }
      break;
    default:
      // BOOST_LOG(rdDebugLog) << "Special 12 " << aid1 << " " << aid4 <<
      // "\n";
      dl = RDGeom::compute14DistCis(bl1, bl2, bl3, ba12, ba23);
      du = RDGeom::compute14DistTrans(bl1, bl2, bl3, ba12, ba23);

      path14.type = Path14Configuration::OTHER;
  }
  if (setTheBound) {
    if (fabs(du - dl) < DIST12_DELTA) {
      dl -= GEN_DIST_TOL;
      du += GEN_DIST_TOL;
    }
    // std::cerr<<"2: "<<aid1<<"-"<<aid4<<std::endl;
    _checkAndSetBounds(aid1, aid4, dl, du, mmat);
    accumData.paths14.push_back(path14);
  }
}

void set14Bounds(const ROMol &mol, DistGeom::BoundsMatPtr mmat,
                 ComputedData &accumData, double *distMatrix,
                 bool useMacrocycle14config, bool forceTransAmides) {
  unsigned int npt = mmat->numRows();
  CHECK_INVARIANT(npt == mol.getNumAtoms(), "Wrong size metric matrix");
  // this is 2.6 million bonds, so it's extremly unlikely to ever occur, but we
  // might as well check:
  const size_t MAX_NUM_BONDS = static_cast<size_t>(
      std::pow(std::numeric_limits<std::uint64_t>::max(), 1. / 3));
  if (mol.getNumBonds() >= MAX_NUM_BONDS) {
    throw ValueErrorException(
        "Too many bonds in the molecule, cannot compute 1-4 bounds");
  }
  const auto rinfo = mol.getRingInfo();  // FIX: make sure we have ring info
  CHECK_INVARIANT(rinfo, "");
  const auto &bondRings = rinfo->bondRings();

  std::unordered_set<unsigned int> bidIsMacrocycle;
  std::unordered_set<std::uint64_t> ringBondPairs;
  std::unordered_set<std::uint64_t> donePaths;
  std::uint64_t nb = mol.getNumBonds();
  // first we will deal with 1-4 atoms that belong to the same ring
  for (const auto &bring : bondRings) {
    const auto rSize = bring.size();
    if (rSize < 3) {
      continue;  // rings with less than 3 bonds are not useful
    }
    auto bid1 = bring[rSize - 1];
    for (auto i = 0u; i < rSize; i++) {
      auto bid2 = bring[i];
      auto bid3 = bring[(i + 1) % rSize];
      auto pid1 = bid1 * nb + bid2;
      auto pid2 = bid2 * nb + bid1;
      auto id1 = bid1 * nb * nb + bid2 * nb + bid3;
      auto id2 = bid3 * nb * nb + bid2 * nb + bid1;

      ringBondPairs.insert(pid1);
      ringBondPairs.insert(pid2);
      donePaths.insert(id1);
      donePaths.insert(id2);

      if (rSize > 5) {
        if (useMacrocycle14config && rSize >= minMacrocycleRingSize) {
          _setMacrocycleAllInSameRing14Bounds(
              mol, mol.getBondWithIdx(bid1), mol.getBondWithIdx(bid2),
              mol.getBondWithIdx(bid3), accumData, mmat, distMatrix);
          bidIsMacrocycle.insert(bid2);
        } else {
          _setInRing14Bounds(mol, mol.getBondWithIdx(bid1),
                             mol.getBondWithIdx(bid2), mol.getBondWithIdx(bid3),
                             accumData, mmat, distMatrix, rSize);
        }
      } else {
        _record14Path(mol, bid1, bid2, bid3, accumData);
      }

      bid1 = bid2;
    }  // loop over bonds in the ring
  }  // end of all rings
  for (const auto bond : mol.bonds()) {
    auto bid2 = bond->getIdx();
    auto aid2 = bond->getBeginAtomIdx();
    auto aid3 = bond->getEndAtomIdx();
    for (const auto bnd1 : mol.atomBonds(mol.getAtomWithIdx(aid2))) {
      auto bid1 = bnd1->getIdx();
      if (bid1 != bid2) {
        for (const auto bnd3 : mol.atomBonds(mol.getAtomWithIdx(aid3))) {
          auto bid3 = bnd3->getIdx();
          if (bid3 != bid2) {
            auto id1 = bid1 * nb * nb + bid2 * nb + bid3;
            auto id2 = bid3 * nb * nb + bid2 * nb + bid1;
            if (donePaths.find(id1) == donePaths.end() &&
                donePaths.find(id2) == donePaths.end()) {
              // we haven't dealt with this path before
              auto pid1 = bid1 * nb + bid2;
              auto pid2 = bid2 * nb + bid1;
              auto pid3 = bid2 * nb + bid3;
              auto pid4 = bid3 * nb + bid2;

              if (ringBondPairs.find(pid1) != ringBondPairs.end() ||
                  ringBondPairs.find(pid2) != ringBondPairs.end() ||
                  ringBondPairs.find(pid3) != ringBondPairs.end() ||
                  ringBondPairs.find(pid4) != ringBondPairs.end()) {
                // either (bid1, bid2) or (bid2, bid3) are in the
                // same ring (note all three cannot be in the same
                // ring; we dealt with that before)
                if (useMacrocycle14config &&
                    bidIsMacrocycle.find(bid2) != bidIsMacrocycle.end()) {
                  _setMacrocycleTwoInSameRing14Bounds(
                      mol, bnd1, bond, bnd3, accumData, mmat, distMatrix);
                } else {
                  _setTwoInSameRing14Bounds(mol, bnd1, bond, bnd3, accumData,
                                            mmat, distMatrix);
                }
              } else if (((rinfo->numBondRings(bid1) > 0) &&
                          (rinfo->numBondRings(bid2) > 0)) ||
                         ((rinfo->numBondRings(bid2) > 0) &&
                          (rinfo->numBondRings(bid3) > 0))) {
                // (bid1, bid2) or (bid2, bid3) are ring bonds but
                // belong to different rings.  Note that the third
                // bond will not belong to either of these two
                // rings (if it does, we would have taken care of
                // it in the previous if block); i.e. if bid1 and
                // bid2 are ring bonds that belong to ring r1 and
                // r2, then bid3 is either an external bond or
                // belongs to a third ring r3.
                _setTwoInDiffRing14Bounds(mol, bnd1, bond, bnd3, accumData,
                                          mmat, distMatrix);
              } else if (rinfo->numBondRings(bid2) > 0) {
                // the middle bond is a ring bond and the other
                // two do not belong to the same ring or are
                // non-ring bonds

                _setShareRingBond14Bounds(mol, bnd1, bond, bnd3, accumData,
                                          mmat, distMatrix);
              } else {
                // middle bond not a ring
                _setChain14Bounds(mol, bnd1, bond, bnd3, accumData, mmat,
                                  distMatrix, forceTransAmides);
              }
            }
          }
        }
      }
    }
  }
}

void initBoundsMat(DistGeom::BoundsMatrix *mmat, double defaultMin,
                   double defaultMax) {
  unsigned int npt = mmat->numRows();

  for (unsigned int i = 1; i < npt; i++) {
    for (unsigned int j = 0; j < i; j++) {
      mmat->setUpperBound(i, j, defaultMax);
      mmat->setLowerBound(i, j, defaultMin);
    }
  }
}
void initBoundsMat(DistGeom::BoundsMatPtr mmat, double defaultMin,
                   double defaultMax) {
  initBoundsMat(mmat.get(), defaultMin, defaultMax);
};

void setTopolBounds(const ROMol &mol, DistGeom::BoundsMatPtr mmat,
                    bool set15bounds, bool scaleVDW, bool useMacrocycle14config,
                    bool forceTransAmides) {
  PRECONDITION(mmat.get(), "bad pointer");
  unsigned int nb = mol.getNumBonds();
  unsigned int na = mol.getNumAtoms();
  if (!na) {
    throw ValueErrorException("molecule has no atoms");
  }
  // this is 2.6 million bonds, so it's extremly unlikely to ever occur, but we
  // might as well check:
  const size_t MAX_NUM_BONDS = static_cast<size_t>(
      std::pow(std::numeric_limits<std::uint64_t>::max(), 1. / 3));
  if (mol.getNumBonds() >= MAX_NUM_BONDS) {
    throw ValueErrorException(
        "Too many bonds in the molecule, cannot compute 1-4 bounds");
  }

  ComputedData accumData(na, nb);
  double *distMatrix = nullptr;
  distMatrix = MolOps::getDistanceMat(mol);

  set12Bounds(mol, mmat, accumData);
  set13Bounds(mol, mmat, accumData);

  set14Bounds(mol, mmat, accumData, distMatrix, useMacrocycle14config,
              forceTransAmides);

  if (set15bounds) {
    set15Bounds(mol, mmat, accumData, distMatrix);
  }

  setLowerBoundVDW(mol, mmat, scaleVDW, distMatrix);
}

void collectBondsAndAngles(const ROMol &mol,
                           std::vector<std::pair<int, int>> &bonds,
                           std::vector<std::vector<int>> &angles) {
  bonds.resize(0);
  angles.resize(0);
  bonds.reserve(mol.getNumBonds());
  for (const auto bondi : mol.bonds()) {
    bonds.emplace_back(bondi->getBeginAtomIdx(), bondi->getEndAtomIdx());

    for (unsigned int j = bondi->getIdx() + 1; j < mol.getNumBonds(); ++j) {
      const Bond *bondj = mol.getBondWithIdx(j);
      int aid11 = bondi->getBeginAtomIdx();
      int aid12 = bondi->getEndAtomIdx();
      int aid21 = bondj->getBeginAtomIdx();
      int aid22 = bondj->getEndAtomIdx();
      if (aid11 != aid21 && aid11 != aid22 && aid12 != aid21 &&
          aid12 != aid22) {
        continue;
      }
      std::vector<int> tmp(4,
                           0);  // elements: aid1, aid2, flag for triple bonds

      if (aid12 == aid21) {
        tmp[0] = aid11;
        tmp[1] = aid12;
        tmp[2] = aid22;
      } else if (aid12 == aid22) {
        tmp[0] = aid11;
        tmp[1] = aid12;
        tmp[2] = aid21;
      } else if (aid11 == aid21) {
        tmp[0] = aid12;
        tmp[1] = aid11;
        tmp[2] = aid22;
      } else if (aid11 == aid22) {
        tmp[0] = aid12;
        tmp[1] = aid11;
        tmp[2] = aid21;
      }

      if (bondi->getBondType() == Bond::TRIPLE ||
          bondj->getBondType() == Bond::TRIPLE) {
        // triple bond
        tmp[3] = 1;
      } else if (bondi->getBondType() == Bond::DOUBLE &&
                 bondj->getBondType() == Bond::DOUBLE &&
                 mol.getAtomWithIdx(tmp[1])->getDegree() == 2) {
        // consecutive double bonds
        tmp[3] = 1;
      }

      angles.push_back(tmp);
    }
  }
}

void setTopolBounds(const ROMol &mol, DistGeom::BoundsMatPtr mmat,
                    std::vector<std::pair<int, int>> &bonds,
                    std::vector<std::vector<int>> &angles, bool set15bounds,
                    bool scaleVDW, bool useMacrocycle14config,
                    bool forceTransAmides) {
  PRECONDITION(mmat.get(), "bad pointer");
  bonds.clear();
  angles.clear();
  unsigned int nb = mol.getNumBonds();
  unsigned int na = mol.getNumAtoms();
  if (!na) {
    throw ValueErrorException("molecule has no atoms");
  }
  ComputedData accumData(na, nb);
  double *distMatrix = nullptr;
  distMatrix = MolOps::getDistanceMat(mol);

  set12Bounds(mol, mmat, accumData);
  set13Bounds(mol, mmat, accumData);
  set14Bounds(mol, mmat, accumData, distMatrix, useMacrocycle14config,
              forceTransAmides);

  if (set15bounds) {
    set15Bounds(mol, mmat, accumData, distMatrix);
  }

  setLowerBoundVDW(mol, mmat, scaleVDW, distMatrix);

  collectBondsAndAngles(mol, bonds, angles);
}

// some helper functions to set 15 distances

/*
 compute the lower and upper bounds for the distance between 15 atoms give
 than
 the first
 four atoms are in cis configuration. The 15 limits are computed assuming the
 following
 configuration
         5
          \
     1     4
      \   /
       2-3
 ARGUMENTS:
   d1 - distance between 1 and 2
   d2 - distance between 2 and 3
   d3 - distance between 3 and 4
   d4 - distance between 4 and 5
   ang12 - angle(123)
   ang23 - angle(234)
   and34 - angle(345)
   dl - storage for lower 15 bound
   du - storage for upper 15 bound
*/
double _compute15DistsCisCis(double d1, double d2, double d3, double d4,
                             double ang12, double ang23, double ang34) {
  double dx14 = d2 - d3 * cos(ang23) - d1 * cos(ang12);
  double dy14 = d3 * sin(ang23) - d1 * sin(ang12);
  double d14 = sqrt(dx14 * dx14 + dy14 * dy14);
  double cval = (d3 - d2 * cos(ang23) + d1 * cos(ang12 + ang23)) / d14;
  if (cval > 1.0) {
    cval = 1.0;
  } else if (cval < -1.0) {
    cval = -1.0;
  }

  double ang143 = acos(cval);
  double ang145 = ang34 - ang143;
  double res = RDGeom::compute13Dist(d14, d4, ang145);
  return res;
}

/*
 compute the lower and upper bounds for the distance between 15 atoms give
 than
 the first
 four atoms are in cis configuration. The 15 limits are computed assuming the
 following
 configuration
  1     4-5
   \   /
    2-3
 ARGUMENTS:
   d1 - distance between 1 and 2
   d2 - distance between 2 and 3
   d3 - distance between 3 and 4
   d4 - distance between 4 and 5
   ang12 - angle(123)
   ang23 - angle(234)
   and34 - angle(345)
   dl - storage for lower 15 bound
   du - storage for upper 15 bound
*/
double _compute15DistsCisTrans(double d1, double d2, double d3, double d4,
                               double ang12, double ang23, double ang34) {
  double dx14 = d2 - d3 * cos(ang23) - d1 * cos(ang12);
  double dy14 = d3 * sin(ang23) - d1 * sin(ang12);
  double d14 = sqrt(dx14 * dx14 + dy14 * dy14);
  double cval = (d3 - d2 * cos(ang23) + d1 * cos(ang12 + ang23)) / d14;
  if (cval > 1.0) {
    cval = 1.0;
  } else if (cval < -1.0) {
    cval = -1.0;
  }

  double ang143 = acos(cval);
  double ang145 = ang34 + ang143;
  return RDGeom::compute13Dist(d14, d4, ang145);
}

/*
 compute the lower and upper bounds for the distance between 15 atoms given
 than
 the first
 four atoms are in trans configuration. The 15 limits are computed assuming
 the
 following
 configuration
  1
   \
    2-3
       \
        4-5
 ARGUMENTS:
   d1 - distance between 1 and 2
   d2 - distance between 2 and 3
   d3 - distance between 3 and 4
   d4 - distance between 4 and 5
   ang12 - angle(123)
   ang23 - angle(234)
   and34 - angle(345)
   dl - storage for lower 15 bound
   du - storage for upper 15 bound
*/
double _compute15DistsTransTrans(double d1, double d2, double d3, double d4,
                                 double ang12, double ang23, double ang34) {
  double dx14 = d2 - d3 * cos(ang23) - d1 * cos(ang12);
  double dy14 = d3 * sin(ang23) + d1 * sin(ang12);
  double d14 = sqrt(dx14 * dx14 + dy14 * dy14);
  double cval = (d3 - d2 * cos(ang23) + d1 * cos(ang12 - ang23)) / d14;
  if (cval > 1.0) {
    cval = 1.0;
  } else if (cval < -1.0) {
    cval = -1.0;
  }

  double ang143 = acos(cval);
  double ang145 = ang34 + ang143;
  return RDGeom::compute13Dist(d14, d4, ang145);
}

/*
 compute the lower and upper bounds for the distance between 15 atoms given
 than
 the first
 four atoms are in trans configuration. The 15 limits are computed assuming
 the
 following
 configuration
                    1
                     \
                      2-3
                         \
                          4
                         /
                        5
 ARGUMENTS:
   d1 - distance between 1 and 2
   d2 - distance between 2 and 3
   d3 - distance between 3 and 4
   d4 - distance between 4 and 5
   ang12 - angle(123)
   ang23 - angle(234)
   and34 - angle(345)
   dl - storage for lower 15 bound
   du - storage for upper 15 bound
*/
double _compute15DistsTransCis(double d1, double d2, double d3, double d4,
                               double ang12, double ang23, double ang34) {
  double dx14 = d2 - d3 * cos(ang23) - d1 * cos(ang12);
  double dy14 = d3 * sin(ang23) + d1 * sin(ang12);
  double d14 = sqrt(dx14 * dx14 + dy14 * dy14);

  double cval = (d3 - d2 * cos(ang23) + d1 * cos(ang12 - ang23)) / d14;
  if (cval > 1.0) {
    cval = 1.0;
  } else if (cval < -1.0) {
    cval = -1.0;
  }

  double ang143 = acos(cval);
  double ang145 = ang34 - ang143;
  return RDGeom::compute13Dist(d14, d4, ang145);
}

void _set15BoundsHelper(const ROMol &mol, unsigned int bid1, unsigned int bid2,
                        unsigned int bid3, unsigned int type,
                        ComputedData &accumData, DistGeom::BoundsMatPtr mmat,
                        double *dmat) {
  unsigned int i, aid1, aid2, aid3, aid4, aid5;
  double d1, d2, d3, d4, ang12, ang23, ang34, du, dl, vw1, vw5;
  unsigned int nb = mol.getNumBonds();
  unsigned int na = mol.getNumAtoms();

  aid2 = accumData.bondAdj->getVal(bid1, bid2);
  aid1 = mol.getBondWithIdx(bid1)->getOtherAtomIdx(aid2);
  aid3 = accumData.bondAdj->getVal(bid2, bid3);
  aid4 = mol.getBondWithIdx(bid3)->getOtherAtomIdx(aid3);
  d1 = accumData.bondLengths[bid1];
  d2 = accumData.bondLengths[bid2];
  d3 = accumData.bondLengths[bid3];
  ang12 = accumData.bondAngles->getVal(bid1, bid2);
  ang23 = accumData.bondAngles->getVal(bid2, bid3);
  for (i = 0; i < nb; i++) {
    du = -1.0;
    dl = 0.0;
    if (accumData.bondAdj->getVal(bid3, i) == static_cast<int>(aid4)) {
      aid5 = mol.getBondWithIdx(i)->getOtherAtomIdx(aid4);
      // make sure we did not com back to the first atom in the path -
      // possible
      // with 4 membered rings
      // this is a fix for Issue 244

      // check that this actually is a 1-5 contact:
      if (dmat[std::max(aid1, aid5) * mmat->numRows() + std::min(aid1, aid5)] <
          3.9) {
        // std::cerr<<"skip: "<<aid1<<"-"<<aid5<<" because
        // d="<<dmat[std::max(aid1,aid5)*mmat->numRows()+std::min(aid1,aid5)]<<std::endl;
        continue;
      }

      if (aid1 != aid5) {  // FIX: do we need this
        unsigned int pid1 = aid1 * na + aid5;
        unsigned int pid2 = aid5 * na + aid1;
        if ((mmat->getLowerBound(aid1, aid5) < DIST12_DELTA) ||
            (accumData.set15Atoms[pid1]) || (accumData.set15Atoms[pid2])) {
          d4 = accumData.bondLengths[i];
          ang34 = accumData.bondAngles->getVal(bid3, i);
          unsigned long pathId =
              static_cast<unsigned long>(bid2) * nb * nb + (bid3)*nb + i;
          if (type == 0) {
            if (accumData.cisPaths.find(pathId) != accumData.cisPaths.end()) {
              dl = _compute15DistsCisCis(d1, d2, d3, d4, ang12, ang23, ang34);
              du = dl + DIST15_TOL;
              dl -= DIST15_TOL;
            } else if (accumData.transPaths.find(pathId) !=
                       accumData.transPaths.end()) {
              dl = _compute15DistsCisTrans(d1, d2, d3, d4, ang12, ang23, ang34);
              du = dl + DIST15_TOL;
              dl -= DIST15_TOL;
            } else {
              dl = _compute15DistsCisCis(d1, d2, d3, d4, ang12, ang23, ang34) -
                   DIST15_TOL;
              du =
                  _compute15DistsCisTrans(d1, d2, d3, d4, ang12, ang23, ang34) +
                  DIST15_TOL;
            }

          } else if (type == 1) {
            if (accumData.cisPaths.find(pathId) != accumData.cisPaths.end()) {
              dl = _compute15DistsTransCis(d1, d2, d3, d4, ang12, ang23, ang34);
              du = dl + DIST15_TOL;
              dl -= DIST15_TOL;
            } else if (accumData.transPaths.find(pathId) !=
                       accumData.transPaths.end()) {
              dl = _compute15DistsTransTrans(d1, d2, d3, d4, ang12, ang23,
                                             ang34);
              du = dl + DIST15_TOL;
              dl -= DIST15_TOL;
            } else {
              dl =
                  _compute15DistsTransCis(d1, d2, d3, d4, ang12, ang23, ang34) -
                  DIST15_TOL;
              du = _compute15DistsTransTrans(d1, d2, d3, d4, ang12, ang23,
                                             ang34) +
                   DIST15_TOL;
            }
          } else {
            if (accumData.cisPaths.find(pathId) != accumData.cisPaths.end()) {
              dl = _compute15DistsCisCis(d4, d3, d2, d1, ang34, ang23, ang12) -
                   DIST15_TOL;
              du =
                  _compute15DistsCisTrans(d4, d3, d2, d1, ang34, ang23, ang12) +
                  DIST15_TOL;
            } else if (accumData.transPaths.find(pathId) !=
                       accumData.transPaths.end()) {
              dl =
                  _compute15DistsTransCis(d4, d3, d2, d1, ang34, ang23, ang12) -
                  DIST15_TOL;
              du = _compute15DistsTransTrans(d4, d3, d2, d1, ang34, ang23,
                                             ang12) +
                   DIST15_TOL;
            } else {
              vw1 = PeriodicTable::getTable()->getRvdw(
                  mol.getAtomWithIdx(aid1)->getAtomicNum());
              vw5 = PeriodicTable::getTable()->getRvdw(
                  mol.getAtomWithIdx(aid5)->getAtomicNum());
              dl = VDW_SCALE_15 * (vw1 + vw5);
            }
          }
          if (du < 0.0) {
            du = MAX_UPPER;
          }

          // std::cerr<<"3: "<<aid1<<"-"<<aid5<<std::endl;
          _checkAndSetBounds(aid1, aid5, dl, du, mmat);
          accumData.set15Atoms[aid1 * na + aid5] = 1;
          accumData.set15Atoms[aid5 * na + aid1] = 1;
        }
      }
    }
  }
}

// set the 15 distance bounds
void set15Bounds(const ROMol &mol, DistGeom::BoundsMatPtr mmat,
                 ComputedData &accumData, double *distMatrix) {
  PATH14_VECT_CI pti;
  unsigned int bid1, bid2, bid3, type;
  for (pti = accumData.paths14.begin(); pti != accumData.paths14.end(); pti++) {
    bid1 = pti->bid1;
    bid2 = pti->bid2;
    bid3 = pti->bid3;
    type = pti->type;
    // 15 distances going one way with with 14 paths
    _set15BoundsHelper(mol, bid1, bid2, bid3, type, accumData, mmat,
                       distMatrix);
    // going the other way - reverse the 14 path
    _set15BoundsHelper(mol, bid3, bid2, bid1, type, accumData, mmat,
                       distMatrix);
  }
}
}  // namespace DGeomHelpers
}  // namespace RDKit
