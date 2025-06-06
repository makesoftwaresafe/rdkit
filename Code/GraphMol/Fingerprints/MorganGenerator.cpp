//
//  Copyright (C) 2018-2025 Boran Adas and other RDKit contributors
//
//   @@ All Rights Reserved @@
//  This file is part of the RDKit.
//  The contents are covered by the terms of the BSD license
//  which is included in the file license.txt, found at the root
//  of the RDKit source tree.
//

#include <GraphMol/RDKitBase.h>
#include <GraphMol/Fingerprints/FingerprintGenerator.h>
#include <GraphMol/Fingerprints/MorganGenerator.h>
#include <RDGeneral/hash/hash.hpp>
#include <GraphMol/SmilesParse/SmilesParse.h>
#include <GraphMol/Substruct/SubstructMatch.h>

#include <RDGeneral/BoostStartInclude.h>
#include <boost/dynamic_bitset.hpp>
#include <RDGeneral/BoostEndInclude.h>
#include <tuple>

#include <GraphMol/Fingerprints/FingerprintUtil.h>
#include <GraphMol/Chirality.h>
#include <GraphMol/CIPLabeler/CIPLabeler.h>

namespace RDKit {
namespace MorganFingerprint {

using namespace MorganFingerprints;

MorganAtomInvGenerator::MorganAtomInvGenerator(const bool includeRingMembership)
    : df_includeRingMembership(includeRingMembership) {}

std::vector<std::uint32_t> *MorganAtomInvGenerator::getAtomInvariants(
    const ROMol &mol) const {
  unsigned int nAtoms = mol.getNumAtoms();
  std::unique_ptr<std::vector<std::uint32_t>> atomInvariants(
      new std::vector<std::uint32_t>(nAtoms));
  getConnectivityInvariants(mol, *atomInvariants, df_includeRingMembership);
  return atomInvariants.release();
}

std::string MorganAtomInvGenerator::infoString() const {
  return "MorganInvariantGenerator includeRingMembership=" +
         std::to_string(df_includeRingMembership);
}

MorganAtomInvGenerator *MorganAtomInvGenerator::clone() const {
  return new MorganAtomInvGenerator(df_includeRingMembership);
}

MorganFeatureAtomInvGenerator::MorganFeatureAtomInvGenerator(
    std::vector<const ROMol *> *patterns) {
  dp_patterns = patterns;
}

std::string MorganFeatureAtomInvGenerator::infoString() const {
  return "MorganFeatureInvariantGenerator";
}

MorganFeatureAtomInvGenerator *MorganFeatureAtomInvGenerator::clone() const {
  return new MorganFeatureAtomInvGenerator(dp_patterns);
}

std::vector<std::uint32_t> *MorganFeatureAtomInvGenerator::getAtomInvariants(
    const ROMol &mol) const {
  unsigned int nAtoms = mol.getNumAtoms();
  std::vector<std::uint32_t> *result = new std::vector<std::uint32_t>(nAtoms);

  getFeatureInvariants(mol, *result, dp_patterns);
  return result;
}

MorganBondInvGenerator::MorganBondInvGenerator(const bool useBondTypes,
                                               const bool useChirality)
    : df_useBondTypes(useBondTypes), df_useChirality(useChirality) {}

std::vector<std::uint32_t> *MorganBondInvGenerator::getBondInvariants(
    const ROMol &mol) const {
  std::vector<std::uint32_t> *result =
      new std::vector<std::uint32_t>(mol.getNumBonds());
  for (unsigned int i = 0; i < mol.getNumBonds(); ++i) {
    Bond const *bond = mol.getBondWithIdx(i);
    int32_t bondInvariant = 1;
    if (df_useBondTypes) {
      if (!df_useChirality || bond->getBondType() != Bond::DOUBLE ||
          bond->getStereo() == Bond::STEREONONE) {
        bondInvariant = static_cast<int32_t>(bond->getBondType());
      } else {
        auto bondStereo = static_cast<int32_t>(bond->getStereo());
        if (!Chirality::getUseLegacyStereoPerception()) {
          // if we aren't using legacy stereo, we need to compute the CIP codes
          if (!mol.hasProp(common_properties::_CIPComputed)) {
            CIPLabeler::assignCIPLabels(const_cast<ROMol &>(mol));
          }

          // for backwards compatibility, if we are E or Z, set those, otherwise
          // just use whatever the bondStereo is set to.
          std::string cipCode;
          if (bond->getPropIfPresent(common_properties::_CIPCode, cipCode)) {
            if (cipCode == "E") {
              bondStereo = static_cast<int32_t>(Bond::STEREOE);
            } else if (cipCode == "Z") {
              bondStereo = static_cast<int32_t>(Bond::STEREOZ);
            }
          }
        }
        const int32_t stereoOffset = 100;
        const int32_t bondTypeOffset = 10;
        bondInvariant =
            stereoOffset +
            bondTypeOffset * static_cast<int32_t>(bond->getBondType()) +
            bondStereo;
      }
    }
    (*result)[bond->getIdx()] = static_cast<int32_t>(bondInvariant);
  }
  return result;
}

std::string MorganBondInvGenerator::infoString() const {
  return "MorganInvariantGenerator useBondTypes=" +
         std::to_string(df_useBondTypes) +
         " useChirality=" + std::to_string(df_useChirality);
}

MorganBondInvGenerator *MorganBondInvGenerator::clone() const {
  return new MorganBondInvGenerator(df_useBondTypes, df_useChirality);
}

template <typename OutputType>
OutputType MorganEnvGenerator<OutputType>::getResultSize() const {
  return std::numeric_limits<OutputType>::max();
}

std::string MorganArguments::infoString() const {
  return "MorganArguments onlyNonzeroInvariants=" +
         std::to_string(df_onlyNonzeroInvariants) +
         " radius=" + std::to_string(d_radius);
}

template <typename OutputType>
void MorganAtomEnv<OutputType>::updateAdditionalOutput(
    AdditionalOutput *additionalOutput, size_t bitId) const {
  PRECONDITION(additionalOutput, "bad output pointer");
  if (additionalOutput->bitInfoMap) {
    (*additionalOutput->bitInfoMap)[bitId].emplace_back(d_atomId, d_layer);
  }
  if (additionalOutput->atomCounts) {
    (*additionalOutput->atomCounts)[d_atomId]++;
  }
  if (additionalOutput->atomToBits) {
    (*additionalOutput->atomToBits)[d_atomId].push_back(bitId);
  }
}

template <typename OutputType>
OutputType MorganAtomEnv<OutputType>::getBitId(
    FingerprintArguments *,              // arguments
    const std::vector<std::uint32_t> *,  // atomInvariants
    const std::vector<std::uint32_t> *,  // bondInvariants
    AdditionalOutput *,                  // additional Output
    const bool,                          // hashResults
    const std::uint64_t                  // fpSize
) const {
  return d_code;
}  // namespace MorganFingerprint

template <typename OutputType>
MorganAtomEnv<OutputType>::MorganAtomEnv(const std::uint32_t code,
                                         const unsigned int atomId,
                                         const unsigned int layer)
    : d_code(code), d_atomId(atomId), d_layer(layer) {}

template <typename OutputType>
std::vector<AtomEnvironment<OutputType> *>
MorganEnvGenerator<OutputType>::getEnvironments(
    const ROMol &mol, FingerprintArguments *arguments,
    const std::vector<std::uint32_t> *fromAtoms,
    const std::vector<std::uint32_t> *,  // ignoreAtoms
    const int,                           // confId
    const AdditionalOutput *,            // additionalOutput
    const std::vector<std::uint32_t> *atomInvariants,
    const std::vector<std::uint32_t> *bondInvariants,
    const bool  // hashResults
) const {
  PRECONDITION(atomInvariants && (atomInvariants->size() >= mol.getNumAtoms()),
               "bad atom invariants size");
  PRECONDITION(bondInvariants && (bondInvariants->size() >= mol.getNumBonds()),
               "bad bond invariants size");
  auto *morganArguments = dynamic_cast<MorganArguments *>(arguments);
  PRECONDITION(morganArguments, "bad arguments type");

  unsigned int nAtoms = mol.getNumAtoms();
  const unsigned int maxNumResults = (morganArguments->d_radius + 1) * nAtoms;

  std::vector<AtomEnvironment<OutputType> *> result =
      std::vector<AtomEnvironment<OutputType> *>();
  result.reserve(maxNumResults);

  // if we are using chirality, we need to make sure the atoms have R/S labels
  if (morganArguments->df_includeChirality &&
      !Chirality::getUseLegacyStereoPerception() &&
      !mol.hasProp(common_properties::_CIPComputed)) {
    CIPLabeler::assignCIPLabels(const_cast<ROMol &>(mol));
  }

  std::vector<OutputType> currentInvariants(atomInvariants->size());
  std::copy(atomInvariants->begin(), atomInvariants->end(),
            currentInvariants.begin());
  // will hold bit ids calculated this round to be used as invariants next
  // round
  std::vector<OutputType> nextLayerInvariants(nAtoms);

  // will hold up to date invariants of neighboring atoms with bond
  // types, these invariants hold information from atoms around radius
  // as big as current layer around the current atom
  std::vector<std::pair<int32_t, uint32_t>> neighborhoodInvariants;
  // Max number of neighbors expected.
  neighborhoodInvariants.reserve(8);

  boost::dynamic_bitset<> includeAtoms(nAtoms);
  if (fromAtoms) {
    for (auto idx : *fromAtoms) {
      includeAtoms.set(idx, 1);
    }
  } else {
    includeAtoms.set();
  }

  boost::dynamic_bitset<> chiralAtoms(nAtoms);

  // these are the neighborhoods that have already been added
  // to the fingerprint
  std::unordered_set<boost::dynamic_bitset<>> neighborhoods;
  neighborhoods.reserve(maxNumResults);
  // these are the environments around each atom:
  std::vector<boost::dynamic_bitset<>> atomNeighborhoods(
      nAtoms, boost::dynamic_bitset<>(mol.getNumBonds()));
  // holds atoms in the environment (neighborhood) for the current layer for
  // each atom, starts with the immediate neighbors of atoms and expands
  // with every iteration
  std::vector<boost::dynamic_bitset<>> roundAtomNeighborhoods =
      atomNeighborhoods;
  boost::dynamic_bitset<> deadAtoms(nAtoms);

  // if df_onlyNonzeroInvariants is set order the atoms to make sure atoms
  // with zero invariants are processed last so that in case of duplicate
  // environments atoms with non-zero invariants are used
  std::vector<unsigned int> atomOrder(nAtoms);
  if (morganArguments->df_onlyNonzeroInvariants) {
    std::vector<std::pair<int32_t, uint32_t>> ordering;
    for (unsigned int i = 0; i < nAtoms; ++i) {
      if (!currentInvariants[i]) {
        ordering.emplace_back(1, i);
      } else {
        ordering.emplace_back(0, i);
      }
    }
    std::sort(ordering.begin(), ordering.end());
    for (unsigned int i = 0; i < nAtoms; ++i) {
      atomOrder[i] = ordering[i].second;
    }
  } else {
    for (unsigned int i = 0; i < nAtoms; ++i) {
      atomOrder[i] = i;
    }
  }

  // add the round 0 invariants to the result
  for (unsigned int i = 0; i < nAtoms; ++i) {
    if (includeAtoms[i]) {
      if (!morganArguments->df_onlyNonzeroInvariants || currentInvariants[i]) {
        result.push_back(
            new MorganAtomEnv<OutputType>(currentInvariants[i], i, 0));
      }
    }
  }

  // now do our subsequent rounds:
  for (unsigned int layer = 0; layer < morganArguments->d_radius; ++layer) {
    std::vector<AccumTuple> allNeighborhoodsThisRound;
    for (auto atomIdx : atomOrder) {
      // skip atoms which will not generate unique environments
      // (neighborhoods) anymore
      if (!deadAtoms[atomIdx]) {
        const Atom *tAtom = mol.getAtomWithIdx(atomIdx);
        if (!tAtom->getDegree()) {
          deadAtoms.set(atomIdx, 1);
          continue;
        }

        ROMol::OEDGE_ITER beg, end;
        boost::tie(beg, end) = mol.getAtomBonds(tAtom);

        // add up to date invariants of neighbors
        // This should keep capacity, so reallocation only triggers if we
        // haven't seen a molecule of this size.
        neighborhoodInvariants.clear();

        while (beg != end) {
          const Bond *bond = mol[*beg];
          roundAtomNeighborhoods[atomIdx][bond->getIdx()] = 1;

          unsigned int oIdx = bond->getOtherAtomIdx(atomIdx);
          roundAtomNeighborhoods[atomIdx] |= atomNeighborhoods[oIdx];

          auto bt = static_cast<int32_t>((*bondInvariants)[bond->getIdx()]);
          neighborhoodInvariants.push_back(
              std::make_pair(bt, currentInvariants[oIdx]));

          ++beg;
        }

        // sort the neighbor list:
        std::sort(neighborhoodInvariants.begin(), neighborhoodInvariants.end());
        // and now calculate the new invariant and test if the atom is newly
        // "chiral"
        std::uint32_t invar = layer;
        gboost::hash_combine(invar, currentInvariants[atomIdx]);
        bool looksChiral = (tAtom->getChiralTag() != Atom::CHI_UNSPECIFIED);
        for (std::vector<std::pair<int32_t, uint32_t>>::const_iterator it =
                 neighborhoodInvariants.begin();
             it != neighborhoodInvariants.end(); ++it) {
          // add the contribution to the new invariant:
          gboost::hash_combine(invar, *it);

          // check our "chirality":
          if (morganArguments->df_includeChirality && looksChiral &&
              !chiralAtoms[atomIdx]) {
            if (it->first != static_cast<int32_t>(Bond::SINGLE)) {
              looksChiral = false;
            } else if (it != neighborhoodInvariants.begin() &&
                       it->second == (it - 1)->second) {
              looksChiral = false;
            }
          }
        }

        if (morganArguments->df_includeChirality && looksChiral) {
          chiralAtoms[atomIdx] = 1;
          // add an extra value to the invariant to reflect chirality:
          std::string cip = "";
          tAtom->getPropIfPresent(common_properties::_CIPCode, cip);
          if (cip == "R") {
            gboost::hash_combine(invar, 3);
          } else if (cip == "S") {
            gboost::hash_combine(invar, 2);
          } else {
            gboost::hash_combine(invar, 1);
          }
        }

        // this rounds bit id will be next rounds atom invariant, so we save
        // it here
        nextLayerInvariants[atomIdx] = static_cast<OutputType>(invar);

        // store the environment that generated this bit id along with the bit
        // id and the atom id
        allNeighborhoodsThisRound.push_back(
            std::make_tuple(roundAtomNeighborhoods[atomIdx],
                            static_cast<OutputType>(invar), atomIdx));
      }
    }

    std::sort(allNeighborhoodsThisRound.begin(),
              allNeighborhoodsThisRound.end());
    for (std::vector<AccumTuple>::const_iterator iter =
             allNeighborhoodsThisRound.begin();
         iter != allNeighborhoodsThisRound.end(); ++iter) {
      // if we haven't seen this exact environment before, add it to the
      // result
      if (morganArguments->df_includeRedundantEnvironments ||
          neighborhoods.count(std::get<0>(*iter)) == 0) {
        if (!morganArguments->df_onlyNonzeroInvariants ||
            (*atomInvariants)[std::get<2>(*iter)]) {
          if (includeAtoms[std::get<2>(*iter)]) {
            result.push_back(new MorganAtomEnv<OutputType>(
                std::get<1>(*iter), std::get<2>(*iter), layer + 1));
            neighborhoods.insert(std::get<0>(*iter));
          }
        }
      } else {
        // we have seen this exact environment before, this atom
        // is now out of consideration:
        deadAtoms[std::get<2>(*iter)] = 1;
      }
    }

    // the invariants from this round become the next round invariants:
    currentInvariants.swap(nextLayerInvariants);
    std::fill(nextLayerInvariants.begin(), nextLayerInvariants.end(), 0);

    // this rounds calculated neighbors will be next rounds initial neighbors,
    // so the radius can grow every iteration
    atomNeighborhoods = roundAtomNeighborhoods;
  }

  return result;
}

template <typename OutputType>
std::string MorganEnvGenerator<OutputType>::infoString() const {
  return "MorganEnvironmentGenerator";
}

template <typename OutputType>
FingerprintGenerator<OutputType> *getMorganGenerator(
    const MorganArguments &args,
    AtomInvariantsGenerator *atomInvariantsGenerator,
    BondInvariantsGenerator *bondInvariantsGenerator, bool ownsAtomInvGen,
    bool ownsBondInvGen) {
  AtomEnvironmentGenerator<OutputType> *morganEnvGenerator =
      new MorganEnvGenerator<OutputType>();

  bool ownsAtomInvGenerator = ownsAtomInvGen;
  if (!atomInvariantsGenerator) {
    atomInvariantsGenerator = new MorganAtomInvGenerator();
    ownsAtomInvGenerator = true;
  }

  bool ownsBondInvGenerator = ownsBondInvGen;
  if (!bondInvariantsGenerator) {
    bondInvariantsGenerator = new MorganBondInvGenerator(
        args.df_useBondTypes, args.df_includeChirality);
    ownsBondInvGenerator = true;
  }

  return new FingerprintGenerator<OutputType>(
      morganEnvGenerator, new MorganArguments(args), atomInvariantsGenerator,
      bondInvariantsGenerator, ownsAtomInvGenerator, ownsBondInvGenerator);
}

template <typename OutputType>
FingerprintGenerator<OutputType> *getMorganGenerator(
    unsigned int radius, bool countSimulation, bool includeChirality,
    bool useBondTypes, bool onlyNonzeroInvariants,
    bool includeRedundantEnvironments,
    AtomInvariantsGenerator *atomInvariantsGenerator,
    BondInvariantsGenerator *bondInvariantsGenerator, std::uint32_t fpSize,
    std::vector<std::uint32_t> countBounds, bool ownsAtomInvGen,
    bool ownsBondInvGen) {
  MorganArguments arguments(radius, countSimulation, includeChirality,
                            onlyNonzeroInvariants, countBounds, fpSize,
                            includeRedundantEnvironments, useBondTypes);

  return getMorganGenerator<OutputType>(arguments, atomInvariantsGenerator,
                                        bondInvariantsGenerator, ownsAtomInvGen,
                                        ownsBondInvGen);
}

template RDKIT_FINGERPRINTS_EXPORT FingerprintGenerator<std::uint32_t> *
getMorganGenerator(const MorganArguments &, AtomInvariantsGenerator *,
                   BondInvariantsGenerator *, bool, bool);

template RDKIT_FINGERPRINTS_EXPORT FingerprintGenerator<std::uint32_t> *
getMorganGenerator(unsigned int radius, bool countSimulation,
                   bool includeChirality, bool useBondTypes,
                   bool onlyNonzeroInvariants,
                   bool includeRedundantEnvironments,
                   AtomInvariantsGenerator *atomInvariantsGenerator,
                   BondInvariantsGenerator *bondInvariantsGenerator,
                   std::uint32_t fpSize, std::vector<std::uint32_t> countBounds,
                   bool ownsAtomInvGen, bool ownsBondInvGen);

template RDKIT_FINGERPRINTS_EXPORT FingerprintGenerator<std::uint64_t> *
getMorganGenerator(unsigned int radius, bool countSimulation,
                   bool includeChirality, bool useBondTypes,
                   bool onlyNonzeroInvariants,
                   bool includeRedundantEnvironments,
                   AtomInvariantsGenerator *atomInvariantsGenerator,
                   BondInvariantsGenerator *bondInvariantsGenerator,
                   std::uint32_t fpSize, std::vector<std::uint32_t> countBounds,
                   bool ownsAtomInvGen, bool ownsBondInvGen);

}  // namespace MorganFingerprint
}  // namespace RDKit
