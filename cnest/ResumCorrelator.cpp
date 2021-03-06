#include "CBiRd.h"
#include <gsl/gsl_integration.h>

#include <gsl/gsl_errno.h>
#include <gsl/gsl_sf_bessel.h>
#include <gsl/gsl_interp.h>
#include <vector>

#define EPSABS 0
#define EPSREL 1e-6

static ParamsResumIntegr integr ;
static double precision_factor ;

//////////////////////////////////////////////////////////////////////

static void myhandler(const char * reason, const char * file,
                                  int line, int gsl_errno) {
	if (gsl_errno == GSL_EROUND) {
		// cerr << "roundoff error occured in IR-convolution:" << endl <<
		// 	"correcting by neighbor points average." << endl << flush ;
	}
	else {
		// just roll: no program abort for users
	}
}

//////////////////////////////////////////////////////////////////////

void ResumCorrelator (const ParamsP11 & params, const size_t & Nq, Coordinates * q, Correlator * Cf, Correlator * Cfsmooth, Correlator * Ps, const size_t & Nlout) {

	gsl_set_error_handler (&myhandler) ;
						// gsl_set_error_handler_off () ;

	size_t NBAO = Nq ;
	double qBAO[NBAO], BAO[NBAO] ;
	for (unsigned i = 0 ; i < Nq ; i++) qBAO[i] = (*q)[i] ;

	// X1, Y1
	integr.f1 = params.f ; 
	//integr.X1Infinity = ResumX1(qInfinity, params) ;
	double X1Table[Nq], Y1Table[Nq] ;
	for (unsigned int m = 0 ; m < NBAO ; m++) {
		X1Table[m] = ResumX1(qBAO[m],params) ;
		Y1Table[m] = ResumY1(qBAO[m],params) ;
	}

	integr.InterpX1.accel = gsl_interp_accel_alloc () ;
	integr.InterpX1.interp = gsl_spline_alloc (gsl_interp_cspline, NBAO) ;
	gsl_spline_init (integr.InterpX1.interp, qBAO, X1Table, NBAO) ;

	integr.InterpY1.accel = gsl_interp_accel_alloc () ;
	integr.InterpY1.interp = gsl_spline_alloc (gsl_interp_cspline, NBAO) ;
	gsl_spline_init (integr.InterpY1.interp, qBAO, Y1Table, NBAO) ;

	// Cf_l interpolators
	integr.InterpCf0.accel = gsl_interp_accel_alloc () ;
	integr.InterpCf0.interp = gsl_spline_alloc (gsl_interp_cspline, NBAO) ;
	integr.InterpCf2.accel = gsl_interp_accel_alloc () ;
	integr.InterpCf2.interp = gsl_spline_alloc (gsl_interp_cspline, NBAO) ;
	integr.InterpCf4.accel = gsl_interp_accel_alloc () ;
	integr.InterpCf4.interp = gsl_spline_alloc (gsl_interp_cspline, NBAO) ;

	// gsl integration stuff
	gsl_integration_workspace * w = gsl_integration_workspace_alloc (100000) ;
	gsl_function F ;
	F.function = &Resum_Integrand_GSL ;

	// IR-convolution
	for (unsigned int n = 0 ; n < Np ; n++) { // loop over Pn

		if (n < 3) integr.o = 1 ; // Q1 for P-linear
		else integr.o = 0 ;	// Q0 for P-1loop

		for (unsigned int i = 0 ; i < Nlout ; i++) { // loop over l = 0,2,4
			integr.l = 2*i ;

			// Interpolation of Cf_lp
			for (unsigned int ii = 0 ; ii < Nq ; ii++) BAO[ii] = (*Cf)[0][n][ii] - (*Cfsmooth)[0][n][ii] ;
			gsl_spline_init (integr.InterpCf0.interp, qBAO, BAO, NBAO) ;
			for (unsigned int ii = 0 ; ii < Nq ; ii++) BAO[ii] = (*Cf)[1][n][ii] - (*Cfsmooth)[1][n][ii] ;
			gsl_spline_init (integr.InterpCf2.interp, qBAO, BAO, NBAO) ;

			vector<int> spikes ; // to correct spikes
			
			for (unsigned int m = 0 ; m < Nout ; m++) { // loop over k to resum

				if (kout[m] <= 0.03) ; // at low k, we do not resum since the resummation does nothing
				else {
					
					integr.k = kout[m] ;
					F.params = &integr ;
					double res = 0., err = 0. ;

					int status = 0 ;
					status = gsl_integration_qag (&F, qMIN, qMAX, EPSABS, EPSREL, 100000, GSL_INTEG_GAUSS61, w, &res, &err) ;

					if (status == GSL_EROUND || status != 0) { 
						cout << status << ": IR-resummation: epsrel decreased to 1e-4 in: " << n << ", " << integr.l << ", " << integr.k << endl ;
						status = gsl_integration_qag (&F, qMIN, qMAX, EPSABS, 1e-4, 100000, GSL_INTEG_GAUSS61, w, &res, &err) ;
					}

					if (status == GSL_EROUND || status != 0) { 
						cout << status << ": IR-resummation: epsrel decreased to 1e-3 in: " << n << ", " << integr.l << ", " << integr.k << endl ;
						status = gsl_integration_qag (&F, qMIN, qMAX, EPSABS, 1e-3, 100000, GSL_INTEG_GAUSS61, w, &res, &err) ;
					}

					if (status == GSL_EROUND || status != 0) { 
						cout << status << ": IR-resummation: epsrel decreased to 1e-2 in: " << n << ", " << integr.l << ", " << integr.k << endl ;
						status = gsl_integration_qag (&F, qMIN, qMAX, EPSABS, 1e-2, 100000, GSL_INTEG_GAUSS61, w, &res, &err) ;
					}

					// creating a spikebug for testing
					//if (k == 0.2) { status = GSL_EROUND ; Pout[i][n][m] = 0. ; } ;
					
					if (status == GSL_EROUND || status != 0) { // For correcting spikes
						cout << status << ": IR-resummation: spline correction in: " << n << ", " << integr.l << ", " << integr.k << endl ;
						bool newspike = true ; // to not store twice the id if two spikes happen for the same k but different lp
						for (unsigned int ii = 0 ; ii < spikes.size() ; ii++) if (spikes[ii] == m) newspike = false ; 
						if (newspike == true) spikes.push_back(m); // save the id of kout where the roundoff error occured
					}
					else (*Ps)[i][n][m] += res ;
				}
			}

			// Correction spikes // where a roundoff error (spike) occured, replacing the spiky value by interpolation through the other points
			if (spikes.empty() == false) {
				size_t N = Nout-spikes.size() ;
				double knospike[N], Pnospike[N] ;
				
				unsigned int mp = 0 ; 
				for (unsigned int m = 0 ; m < Nout ; m++) { // filling knospike and Pnospike array with the points where no spike occured...
					bool isspike = false ; 
					for (unsigned int ii = 0 ; ii < spikes.size() ; ii++) if (spikes[ii] == m) isspike = true ; 
					if (isspike == false) { knospike[mp] = kout[m] ; Pnospike[mp] = (*Ps)[i][n][m] ; mp ++ ; } 
				}

				InterpFunc InterpNoSpike ; // ... for interpolation
				InterpNoSpike.accel = gsl_interp_accel_alloc () ;
				InterpNoSpike.interp = gsl_spline_alloc (gsl_interp_cspline, N) ;
				gsl_spline_init (InterpNoSpike.interp, knospike, Pnospike, N) ;
				
				for (unsigned int ii = 0 ; ii < spikes.size() ; ii++) (*Ps)[i][n][spikes[ii]] = C(kout[spikes[ii]], InterpNoSpike) ; // replacing the spikes by interpolated values
				
				UnloadInterp(InterpNoSpike) ;
			}
		}
	}

	gsl_integration_workspace_free (w) ;

	UnloadInterp (integr.InterpCf0) ;
	UnloadInterp (integr.InterpCf2) ;
	UnloadInterp (integr.InterpCf4) ;

	UnloadInterp(integr.InterpX1) ;
	UnloadInterp(integr.InterpY1) ;
}

//////////////////////////////////////////////////////////////////////

double Resum_Integrand_GSL (double q, void * params) {
	ParamsResumIntegr p = *(ParamsResumIntegr *) params ;

	switch (p.l) {
		case 0:
			return C(q,p.InterpCf0) * ( Q(p.o, p.l, 0, p.k, q, p.InterpX1, p.InterpY1, p.f1) - 4.*M_PI*pow(q,2) * gsl_sf_bessel_j0(q * p.k) )
			 	 + C(q,p.InterpCf2) * Q(p.o, p.l, 2, p.k, q, p.InterpX1, p.InterpY1, p.f1) ;
			 	 //+ C(q,p.InterpCf4) * Q(p.o, p.l, 4, p.k, q, p.InterpX1, p.InterpY1, p.f1) ;
			break ;
		case 2:
			return 1.* C(q,p.InterpCf0) * Q(p.o, p.l, 0, p.k, q, p.InterpX1, p.InterpY1, p.f1)
				 + C(q,p.InterpCf2) * ( Q(p.o, p.l, 2, p.k, q, p.InterpX1, p.InterpY1, p.f1) - 4.*M_PI*pow(q,2)*gsl_sf_bessel_j2(q * p.k) ) ;
				 //+ C(q,p.InterpCf4) * Q(p.o, p.l, 4, p.k, q, p.InterpX1, p.InterpY1, p.f1) ;
			break ;
		case 4:
			return C(q,p.InterpCf0) * Q(p.o, p.l, 0, p.k, q, p.InterpX1, p.InterpY1, p.f1)
				 + C(q,p.InterpCf2) * Q(p.o, p.l, 2, p.k, q, p.InterpX1, p.InterpY1, p.f1)
				 + C(q,p.InterpCf4) * ( Q(p.o, p.l, 4, p.k, q, p.InterpX1, p.InterpY1, p.f1) - 4.*M_PI*pow(q,2)*gsl_sf_bessel_jl(4, q * p.k) ) ;
			break ;
	}

	//return pow(q,2) * C(q,p.InterpCf) * gsl_sf_bessel_j0(q * p.k) ;
}

//////////////////////////////////////////////////////////////////////

double C (const double & q, const InterpFunc & c) {
	return gsl_spline_eval (c.interp, q, c.accel) ;
}

double Q (const int & order, const int & l, const int & lp, const double & k, const double & q, const InterpFunc & InterpX1, const InterpFunc & InterpY1, const double & ff) {
	if (order == 0) return ResumQ0(l, lp, k, q, InterpX1, InterpY1, ff) ;
	else return ResumQ1(l, lp, k, q, InterpX1, InterpY1, ff) ;
}

//////////////////////////////////////////////////////////////////////

void UnloadInterp (InterpFunc & params) {
	gsl_spline_free (params.interp) ;
    gsl_interp_accel_free (params.accel) ;
}