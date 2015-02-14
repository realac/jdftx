/*-------------------------------------------------------------------
Copyright 2015 Ravishankar Sundararaman

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

#include <electronic/ElectronScattering.h>
#include <electronic/Everything.h>
#include <electronic/ColumnBundle.h>
#include <electronic/ColumnBundleTransform.h>
#include <core/LatticeUtils.h>
#include <core/Random.h>

matrix operator*(const matrix& m, const std::vector<complex>& d)
{	assert(m.nCols()==int(d.size()));
	matrix ret(m); //copy input to out
	//Scale each column:
	for(int j=0; j<ret.nCols(); j++)
		callPref(eblas_zscal)(ret.nRows(), d[j], ret.dataPref()+ret.index(0,j), 1);
	return ret;
}

//Extract imaginary part:
matrix imag(const matrix& m)
{	matrix out = zeroes(m.nRows(), m.nCols());
	callPref(eblas_daxpy)(m.nData(), 1., ((const double*)m.dataPref())+1,2, (double*)out.dataPref(),2); //copy with stride of 2 to extract imaginary part
	return out;
}

ElectronScattering::ElectronScattering()
: eta(0.), Ecut(0.), fCut(1e-6), omegaMax(0.)
{
}

void ElectronScattering::dump(const Everything& everything)
{	Everything& e = (Everything&)everything; //may modify everything to save memory / optimize
	this->e = &everything;
	nBands = e.eInfo.nBands;
	nSpinor = e.eInfo.spinorLength();
	
	logPrintf("\n----- Electron-electron scattering Im(Sigma) -----\n"); logFlush();

	//Update default parameters:
	if(!eta)
	{	eta = e.eInfo.kT;
		if(!eta) die("eta must be specified explicitly since electronic temperature is zero.\n");
	}
	if(!Ecut) Ecut = e.cntrl.Ecut;
	double oMin = DBL_MAX, oMax = -DBL_MAX; //occupied energy range
	double uMin = DBL_MAX, uMax = -DBL_MAX; //unoccupied energy range
	for(int q=e.eInfo.qStart; q<e.eInfo.qStop; q++)
		for(int b=0; b<nBands; b++)
		{	double E = e.eVars.Hsub_eigs[q][b];
			double f = e.eVars.F[q][b];
			if(f > fCut) //sufficiently occupied
			{	oMin = std::min(oMin, E);
				oMax = std::max(oMax, E);
			}
			if(f < 1.-fCut) //sufficiently unoccupied
			{	uMin = std::min(uMin, E);
				uMax = std::max(uMax, E);
			}
		}
	mpiUtil->allReduce(oMin, MPIUtil::ReduceMin);
	mpiUtil->allReduce(oMax, MPIUtil::ReduceMax);
	mpiUtil->allReduce(uMin, MPIUtil::ReduceMin);
	mpiUtil->allReduce(uMax, MPIUtil::ReduceMax);
	if(!omegaMax) omegaMax = std::max(uMax-uMin, oMax-oMin);
	Emin = uMin - omegaMax;
	Emax = oMax + omegaMax;
	//--- print selected values after fixing defaults:
	logPrintf("Frequency resolution:    %lg\n", eta);
	logPrintf("Dielectric matrix Ecut:  %lg\n", Ecut);
	logPrintf("Maximum energy transfer: %lg\n", omegaMax);
	
	//Initialize frequency grid:
	diagMatrix omegaGrid, wOmega;
	omegaGrid.push_back(0.);
	wOmega.push_back(0.5*eta); //integration weight (halved at endpoint)
	while(omegaGrid.back()<omegaMax + 10*eta) //add margin for covering enough of the Lorentzians
	{	omegaGrid.push_back(omegaGrid.back() + eta);
		wOmega.push_back(eta);
	}
	int iOmegaStart, iOmegaStop; //split dielectric computation over frequency grid
	TaskDivision omegaDiv(omegaGrid.size(), mpiUtil);
	omegaDiv.myRange(iOmegaStart, iOmegaStop);
	logPrintf("Initialized frequency grid with resolution %lg and %d points.\n", eta, omegaGrid.nRows());

	//Make necessary quantities available on all processes:
	C.resize(e.eInfo.nStates);
	E.resize(e.eInfo.nStates);
	F.resize(e.eInfo.nStates);
	for(int q=0; q<e.eInfo.nStates; q++)
	{	int procSrc = e.eInfo.whose(q);
		if(procSrc == mpiUtil->iProcess())
		{	std::swap(C[q], e.eVars.C[q]);
			std::swap(E[q], e.eVars.Hsub_eigs[q]);
			std::swap(F[q], e.eVars.F[q]);
		}
		else
		{	C[q].init(nBands, e.basis[q].nbasis * nSpinor, &e.basis[q], &e.eInfo.qnums[q]);
			E[q].resize(nBands);
			F[q].resize(nBands);
		}
		C[q].bcast(procSrc);
		E[q].bcast(procSrc);
		F[q].bcast(procSrc);
	}
	
	//Randomize supercell to improve load balancing on k-mesh:
	{	std::vector< vector3<> >& kmesh = e.coulombParams.supercell->kmesh;
		std::vector<Supercell::KmeshTransform>& kmeshTransform = e.coulombParams.supercell->kmeshTransform;
		for(size_t ik=0; ik<kmesh.size()-1; ik++)
		{	size_t jk = ik + floor(Random::uniform(kmesh.size()-ik));
			mpiUtil->bcast(jk);
			if(jk !=ik && jk < kmesh.size())
			{	std::swap(kmesh[ik], kmesh[jk]);
				std::swap(kmeshTransform[ik], kmeshTransform[jk]);
			}
		}
	}
	
	//Report maximum nearest-neighbour eigenvalue change (to guide choice of eta)
	supercell = e.coulombParams.supercell;
	matrix3<> kBasisT = inv(supercell->Rsuper) * e.gInfo.R;
	vector3<> kBasis[3]; for(int j=0; j<3; j++) kBasis[j] = kBasisT.row(j);
	plook = std::make_shared< PeriodicLookup< vector3<> > >(supercell->kmesh, e.gInfo.GGT);
	size_t ikStart, ikStop;
	TaskDivision(supercell->kmesh.size(), mpiUtil).myRange(ikStart, ikStop);
	double dEmax = 0.;
	for(size_t ik=ikStart; ik<ikStop; ik++)
	{	const diagMatrix& Ei = E[supercell->kmeshTransform[ik].iReduced];
		for(int j=0; j<3; j++)
		{	size_t jk = plook->find(supercell->kmesh[ik] + kBasis[j]);
			assert(jk != string::npos);
			const diagMatrix& Ej = E[supercell->kmeshTransform[jk].iReduced];
			for(int b=0; b<nBands; b++)
				if(Emin <= Ei[b] && Ei[b] <= Emax)
					dEmax = std::max(dEmax, fabs(Ej[b]-Ei[b]));
		}
	}
	mpiUtil->allReduce(dEmax, MPIUtil::ReduceMax);
	logPrintf("Maximum k-neighbour dE: %lg (guide for selecting eta)\n", dEmax);
	
	//Initialize quantum numbers:
	qnumMesh.resize(supercell->kmesh.size());
	double kWeight = double(e.eInfo.spinWeight) / qnumMesh.size();
	for(size_t ik=0; ik<qnumMesh.size(); ik++)
	{	qnumMesh[ik].k = supercell->kmesh[ik];
		qnumMesh[ik].spin = 0;
		qnumMesh[ik].weight = kWeight;
	}
	
	//Initialize reduced q-Mesh:
	//--- q-mesh is a k-point dfference mesh, which could differ from k-mesh for off-Gamma meshes
	qmesh.resize(supercell->kmesh.size());
	for(size_t iq=0; iq<qmesh.size(); iq++)
	{	qmesh[iq].k = supercell->kmesh[iq] - supercell->kmesh[0]; //k-difference
		qmesh[iq].weight = 1./qmesh.size(); //uniform mesh
		qmesh[iq].spin = 0;
	}
	logPrintf("Symmetries reduced momentum transfers (q-mesh) from %d to ", int(qmesh.size()));
	qmesh = e.symm.reduceKmesh(qmesh);
	logPrintf("%d entries\n", int(qmesh.size())); logFlush();
	
	//Initialize polarizability/dielectric bases corresponding to qmesh:
	logPrintf("Setting up reduced polarizability bases at Ecut = %lg: ", Ecut); logFlush();
	basisChi.resize(qmesh.size());
	double avg_nbasis = 0.;
	const GridInfo& gInfoBasis = e.gInfoWfns ? *e.gInfoWfns : e.gInfo;
	logSuspend();
	for(size_t iq=0; iq<qmesh.size(); iq++)
	{	basisChi[iq].setup(gInfoBasis, e.iInfo, Ecut, qmesh[iq].k);
		avg_nbasis += qmesh[iq].weight * basisChi[iq].nbasis;
	}
	logResume();
	logPrintf("nbasis = %.2lf average, %.2lf ideal\n", avg_nbasis, pow(sqrt(2*Ecut),3)*(e.gInfo.detR/(6*M_PI*M_PI)));
	logFlush();

	//Initialize common wavefunction basis and ColumnBundle transforms for full k-mesh:
	logPrintf("Setting up k-mesh wavefunction transforms ... "); logFlush();
	double kMaxSq = 0;
	for(const vector3<>& k: supercell->kmesh)
		kMaxSq = std::max(kMaxSq, e.gInfo.GGT.metric_length_squared(k));
	double GmaxEff = sqrt(2.*e.cntrl.Ecut) + sqrt(kMaxSq);
	double EcutEff = 0.5*GmaxEff*GmaxEff * (1.+symmThreshold); //add some margin for round-off error safety
	logSuspend();
	basis.setup(e.gInfo, e.iInfo, EcutEff, vector3<>());
	logResume();
	ColumnBundleTransform::BasisWrapper basisWrapper(basis);
	std::vector<matrix3<int>> sym = e.symm.getMatrices();
	transform.resize(supercell->kmesh.size());
	for(size_t ik=0; ik<supercell->kmesh.size(); ik++)
	{	const Supercell::KmeshTransform& kTransform = supercell->kmeshTransform[ik];
		const Basis& basisC = e.basis[kTransform.iReduced];
		const vector3<>& kC = e.eInfo.qnums[kTransform.iReduced].k;
		transform[ik] = std::make_shared<ColumnBundleTransform>(kC, basisC, supercell->kmesh[ik], basisWrapper,
			nSpinor, sym[kTransform.iSym], kTransform.invert);
	}
	logPrintf("done.\n"); logFlush();

	//Main loop over momentum transfers:
	diagMatrix ImKscrHead(omegaGrid.size(), 0.);
	std::vector<diagMatrix> ImSigma(e.eInfo.nStates, diagMatrix(nBands,0.));
	for(size_t iq=0; iq<qmesh.size(); iq++)
	{	logPrintf("\nMomentum transfer %d of %d: q = ", int(iq+1), int(qmesh.size()));
		qmesh[iq].k.print(globalLog, " %+.5lf ");
		
		//Construct Coulomb operator (regularizes G=0 using the tricks developed for EXX):
		matrix invKq = inv(coulombMatrix(iq));
		
		//Calculate chi_KS:
		std::vector<matrix> chiKS(omegaGrid.nRows());
		logPrintf("\tComputing chi_KS ...  "); logFlush(); 
		size_t nkMine = ikStop-ikStart;
		int ikInterval = std::max(1, int(round(nkMine/20.))); //interval for reporting progress
		for(size_t ik=ikStart; ik<ikStop; ik++)
		{	//Report progress:
			size_t ikDone = ik-ikStart+1;
			if(ikDone % ikInterval == 0)
			{	logPrintf("%d%% ", int(round(ikDone*100./nkMine)));
				logFlush();
			}
			//Get events:
			size_t jk; matrix nij;
			std::vector<Event> events = getEvents(true, ik, iq, jk, nij);
			if(!events.size()) continue;
			//Collect contributions for each frequency:
			for(int iOmega=0; iOmega<omegaGrid.nRows(); iOmega++)
			{	double omega = omegaGrid[iOmega];
				complex omegaTilde(omega, 2*eta);
				complex one(1,0);
				std::vector<complex> Xks; Xks.reserve(events.size());
				for(const Event& event: events)
					Xks.push_back(-e.gInfo.detR * kWeight * event.fWeight
						* (one/(event.Eji - omegaTilde) + one/(event.Eji + omegaTilde)) );
				chiKS[iOmega] += (nij * Xks) * dagger(nij);
			}
		}
		for(int iOmega=0; iOmega<omegaGrid.nRows(); iOmega++)
			chiKS[iOmega].allReduce(MPIUtil::ReduceSum);
		logPrintf("done.\n"); logFlush();
		
		//Figure out head entry index:
		int iHead = -1, nbasis = basisChi[iq].nbasis;
		for(size_t n=0; n<basisChi[iq].nbasis; n++)
			if(!basisChi[iq].iGarr[n].length_squared())
			{	iHead = n;
				break;
			}
		assert(iHead >= 0);
		
		//Calculate Im(screened Coulomb operator):
		logPrintf("\tComputing Im(Kscreened) ... "); logFlush();
		std::vector<matrix> ImKscr(omegaGrid.nRows(), zeroes(nbasis, nbasis));
		for(int iOmega=iOmegaStart; iOmega<iOmegaStop; iOmega++)
		{	ImKscr[iOmega] = imag(inv(invKq - chiKS[iOmega]));
			chiKS[iOmega] = 0; //free to save memory
			ImKscrHead[iOmega] += qmesh[iq].weight * ImKscr[iOmega](iHead,iHead).real(); //accumulate head of ImKscr
		}
		for(int iOmega=0; iOmega<omegaGrid.nRows(); iOmega++)
			ImKscr[iOmega].bcast(omegaDiv.whose(iOmega));
		chiKS.clear();
		logPrintf("done.\n"); logFlush();
		
		//Calculate ImSigma contributions:
		logPrintf("\tComputing ImSigma ... "); logFlush(); 
		for(size_t ik=ikStart; ik<ikStop; ik++)
		{	//Report progress:
			size_t ikDone = ik-ikStart+1;
			if(ikDone % ikInterval == 0)
			{	logPrintf("%d%% ", int(round(ikDone*100./nkMine)));
				logFlush();
			}
			//Get events:
			size_t jk; matrix nij;
			std::vector<Event> events = getEvents(false, ik, iq, jk, nij);
			if(!events.size()) continue;
			//Integrate over frequency for event contributions to linewidth:
			diagMatrix eventContrib(events.size(), 0);
			for(int iOmega=0; iOmega<omegaGrid.nRows(); iOmega++)
			{	//Construct energy conserving delta-function:
				double omega = omegaGrid[iOmega];
				complex omegaTilde(omega, 2*eta);
				diagMatrix delta; delta.reserve(events.size());
				for(const Event& event: events)
					delta.push_back(e.gInfo.detR * event.fWeight //overlap and sign for electron / hole
						* (2*eta/M_PI) * ( 1./(event.Eji - omegaTilde).norm() - 1./(event.Eji + omegaTilde).norm()) ); //Normalized Lorentzians
				eventContrib += wOmega[iOmega] * delta * diag(dagger(nij) * ImKscr[iOmega] * nij);
			}
			//Accumulate contributions to linewidth:
			int iReduced = supercell->kmeshTransform[ik].iReduced; //directly collect to reduced k-point
			double symFactor = e.eInfo.spinWeight / (supercell->kmesh.size() * e.eInfo.qnums[iReduced].weight); //symmetrization factor = 1 / |orbit of iReduced|
			double qWeight = qmesh[iq].weight;
			for(size_t iEvent=0; iEvent<events.size(); iEvent++)
			{	const Event& event = events[iEvent];
				ImSigma[iReduced][event.i] += symFactor * qWeight * eventContrib[iEvent];
			}
		}
		logPrintf("done.\n"); logFlush();
	}
	logPrintf("\n");
	
	ImKscrHead.allReduce(MPIUtil::ReduceSum);
	for(diagMatrix& IS: ImSigma)
		IS.allReduce(MPIUtil::ReduceSum);
	for(int q=0; q<e.eInfo.nStates; q++)
		for(int b=0; b<nBands; b++)
		{	double Eqb = E[q][b];
			if(Eqb<Emin || Eqb>Emax)
				ImSigma[q][b] = NAN; //clearly mark as invalid
		}
	
	string fname = e.dump.getFilename("ImSigma_ee");
	logPrintf("Dumping %s ... ", fname.c_str()); logFlush();
	e.eInfo.write(ImSigma, fname.c_str());
	logPrintf("done.\n");

	fname = e.dump.getFilename("ImKscrHead");
	logPrintf("Dumping %s ... ", fname.c_str()); logFlush();
	FILE* fp = fopen(fname.c_str(), "w");
	for(int iOmega=0; iOmega<omegaGrid.nRows(); iOmega++)
		fprintf(fp, "%lf %le\n", omegaGrid[iOmega], ImKscrHead[iOmega]);
	fclose(fp);
	logPrintf("done.\n");

	logPrintf("\n"); logFlush();
}


std::vector<ElectronScattering::Event> ElectronScattering::getEvents(bool chiMode, size_t ik, size_t iq, size_t& jk, matrix& nij) const
{	static StopWatch watchI("ElectronScattering::getEventsI"), watchJ("ElectronScattering::getEventsJ");
	//Find target k-point:
	const vector3<>& ki = supercell->kmesh[ik];
	const vector3<> kj = ki + qmesh[iq].k;
	jk = plook->find(kj);
	assert(jk != string::npos);
	
	//Compile list of events:
	int iReduced = supercell->kmeshTransform[ik].iReduced;
	int jReduced = supercell->kmeshTransform[jk].iReduced;
	const diagMatrix &Ei = E[iReduced], &Fi = F[iReduced];
	const diagMatrix &Ej = E[jReduced], &Fj = F[jReduced];
	std::vector<Event> events; events.reserve((nBands*nBands)/2);
	std::vector<bool> iUsed(nBands,false), jUsed(nBands,false); //sets of i and j actually referenced
	Event event;
	for(event.i=0; event.i<nBands; event.i++)
	for(event.j=0; event.j<nBands; event.j++)
	{	event.fWeight = chiMode ? 0.5*(Fi[event.i] - Fj[event.j]) : (1. - Fi[event.i] - Fj[event.j]);
		double Eii = Ei[event.i];
		double Ejj = Ej[event.j];
		event.Eji = Ejj - Eii;
		if(!chiMode)
		{	if(Eii<Emin || Eii>Emax) event.fWeight = 0.; //state out of relevant range
			if(event.fWeight * (Eii-Ejj) <= 0) event.fWeight = 0; //wrong sign for energy transfer
		}
		if(fabs(event.fWeight) > fCut)
		{	events.push_back(event);
			iUsed[event.i] = true;
			jUsed[event.j] = true;
		}
	}
	if(!events.size()) return events;
	
	//Get wavefunctions in real space:
	ColumnBundle Ci = getWfns(ik), Cj = getWfns(jk);
	std::vector< std::vector<complexDataRptr> > conjICi(nBands), ICj(nBands);
	watchI.start();
	for(int i=0; i<nBands; i++) if(iUsed[i])
	{	conjICi[i].resize(nSpinor);
		for(int s=0; s<nSpinor; s++)
			conjICi[i][s] = conj(I(Ci.getColumn(i,s))); 
	}
	for(int j=0; j<nBands; j++) if(jUsed[j])
	{	ICj[j].resize(nSpinor);
		for(int s=0; s<nSpinor; s++)
			ICj[j][s] = I(Cj.getColumn(j,s));
	}
	watchI.stop();
	
	//Initialize pair densities:
	watchJ.start();
	const Basis& basis_q = basisChi[iq];
	nij = zeroes(basis_q.nbasis, events.size());
	complex* nijData = nij.dataPref();
	for(const Event& event: events)
	{	complexDataRptr Inij;
		for(int s=0; s<nSpinor; s++)
			Inij += conjICi[event.i][s] * ICj[event.j][s];
		callPref(eblas_gather_zdaxpy)(basis_q.nbasis, 1., basis_q.indexPref, J(Inij)->dataPref(), nijData);
		nijData += basis_q.nbasis;
	}
	watchJ.stop();
	
	return events;
}

ColumnBundle ElectronScattering::getWfns(size_t ik) const
{	static StopWatch watch("ElectronScattering::getWfns"); watch.start();
	ColumnBundle result(nBands, basis.nbasis * nSpinor, &basis, &qnumMesh[ik], isGpuEnabled());
	result.zero();
	transform[ik]->scatterAxpy(1., C[supercell->kmeshTransform[ik].iReduced], result,0,1);
	watch.stop();
	return result;
}

matrix ElectronScattering::coulombMatrix(size_t iq) const
{	//Use function implemented in Polarizability:
	matrix coulombMatrix(const ColumnBundle& V, const Everything& e, vector3<> dk);
	const Basis& basis_q = basisChi[iq];
	ColumnBundle V(basis_q.nbasis, basis_q.nbasis, &basis_q);
	V.zero();
	complex* Vdata = V.data();
	double normFac = 1./sqrt(e->gInfo.detR);
	for(size_t b=0; b<basis_q.nbasis; b++)
		Vdata[V.index(b,b)] = normFac;
	return coulombMatrix(V, *e, qmesh[iq].k);
}
