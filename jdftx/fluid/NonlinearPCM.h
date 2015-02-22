/*-------------------------------------------------------------------
Copyright 2011 Ravishankar Sundararaman, Kendra Letchworth Weaver, Deniz Gunceler

This file is part of JDFTx.

JDFTx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

JDFTx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with JDFTx.  If not, see <http://www.gnu.org/licenses/>.
-------------------------------------------------------------------*/


#ifndef JDFTX_ELECTRONIC_NONLINEARPCM_H
#define JDFTX_ELECTRONIC_NONLINEARPCM_H

#include <fluid/PCM.h>
#include <core/VectorField.h>
#include <core/Minimize.h>


namespace NonlinearPCMeval { struct Screening; struct Dielectric; } //Forward declaration of helper classes

typedef ScalarFieldMultiplet<ScalarFieldData,5> ScalarFieldDataMuEps;


class NonlinearPCM : public PCM, public Minimizable<ScalarFieldDataMuEps>
{
public:
	ScalarFieldDataMuEps state; //!< State of the solver = the total electrostatic potential

	//! See createFluidSolver()
	NonlinearPCM(const Everything& e, const FluidSolverParams& params);
    virtual ~NonlinearPCM();
	
	bool needsGummel() { return true; }

	void loadState(const char* filename); //!< Load state from file
	void saveState(const char* filename) const; //!< Save state to file
	void dumpDensities(const char* filenamePattern) const;
	void minimizeFluid(); //!< Converge using nonlinear conjugate gradients

	//! Compute gradient and free energy (with optional outputs)
	double operator()(const ScalarFieldDataMuEps& state, ScalarFieldDataMuEps& Adiel_state, ScalarFieldTilde* Adiel_rhoExplicitTilde=0, ScalarFieldTilde* Adiel_nCavityTilde=0, bool electricOnly=false) const;

	// Interface for Minimizable:
	void step(const ScalarFieldDataMuEps& dir, double alpha);
	double compute(ScalarFieldDataMuEps* grad);
	ScalarFieldDataMuEps precondition(const ScalarFieldDataMuEps& in);

protected:
	void set_internal(const ScalarFieldTilde& rhoExplicitTilde, const ScalarFieldTilde& nCavityTilde);
	double get_Adiel_and_grad_internal(ScalarFieldTilde& Adiel_rhoExplicitTilde, ScalarFieldTilde& Adiel_nCavityTilde, IonicGradient* extraForces, bool electricOnly) const;

private:
	double pMol, ionNbulk, ionZ;
	NonlinearPCMeval::Screening* screeningEval; //! Internal helper class for Screening from PCM_internal
	NonlinearPCMeval::Dielectric* dielectricEval; //! Internal helper class for Dielectric from PCM_internal
	RadialFunctionG preconditioner;
};

#endif // JDFTX_ELECTRONIC_NONLINEARPCM_H
