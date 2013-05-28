/************ TabPivot C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: TABPIVOT                                              */
/* -------------                                                       */
/*  Version 1.6                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2013    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the PIVOT classes DB execution routines.          */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the operating system header file.     */
/***********************************************************************/
#include "my_global.h"
#if defined(WIN32)
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif
//#include <windows.h>
#elif defined(UNIX)
#include <errno.h>
#include <unistd.h>
#include "osutil.h"
#else
#include <io.h>
#endif

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/***********************************************************************/
#define FRM_VER 6
#include "table.h"       // MySQL table definitions
#include "sql_const.h"
#include "field.h"
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "tabcol.h"
#include "colblk.h"
#include "tabmysql.h"
#include "csort.h"
#include "tabutil.h"
#include "tabpivot.h"
#include "valblk.h"
#include "ha_connect.h"
#include "mycat.h"       // For GetHandler

extern "C" int trace;

/***********************************************************************/
/*  Make the Pivot table column list.                                  */
/***********************************************************************/
PQRYRES PivotColumns(PGLOBAL g, const char *tab,   const char *src, 
                                const char *picol, const char *fncol,
                                const char *host,  const char *db,
                                const char *user,  const char *pwd,
                                int port)
  {
  PIVAID pvd(tab, src, picol, fncol, host, db, user, pwd, port);

  return pvd.MakePivotColumns(g);
  } // end of PivotColumns

/* --------------- Implementation of the PIVAID classe --------------- */

/***********************************************************************/
/*  PIVAID constructor.                                                */
/***********************************************************************/
PIVAID::PIVAID(const char *tab,   const char *src,  const char *picol,
               const char *fncol, const char *host, const char *db,
               const char *user,  const char *pwd,  int port)
      : CSORT(false)
  {
  Host = (char*)host;
  User = (char*)user;
  Pwd = (char*)pwd;
  Qryp = NULL;
  Database = (char*)db;
  Tabname = (char*)tab;
  Tabsrc = (char*)src;
  Picol = (char*)picol;
  Fncol = (char*)fncol;
  Rblkp = NULL;
  Port = (port) ? port : GetDefaultPort();
  } // end of PIVAID constructor

/***********************************************************************/
/*  Make the Pivot table column list.                                  */
/***********************************************************************/
PQRYRES PIVAID::MakePivotColumns(PGLOBAL g)
  {
  char    *query, *colname, buf[32];
  int      ndif, nblin, w = 0;
  PVAL     valp;
  PCOLRES *pcrp, crp, fncrp = NULL;

  if (!Tabsrc && Tabname) {
    // Locate the  query
    query = (char*)PlugSubAlloc(g, NULL, strlen(Tabname) + 16);
    sprintf(query, "SELECT * FROM %s", Tabname);
  } else if (!Tabsrc) {
    strcpy(g->Message, MSG(SRC_TABLE_UNDEF));
    return NULL;
  } else
    query = Tabsrc;

  // Open a MySQL connection for this table
  if (Myc.Open(g, Host, Database, User, Pwd, Port))
    return NULL;

  // Send the source command to MySQL
  if (Myc.ExecSQL(g, query, &w) == RC_FX) {
    Myc.Close();
    return NULL;
    } // endif Exec

  // We must have a storage query to get pivot column values
  Qryp = Myc.GetResult(g);
  Myc.Close();

  if (!Fncol) {
    for (crp = Qryp->Colresp; crp; crp = crp->Next)
      if (!Picol || stricmp(Picol, crp->Name))
        Fncol = crp->Name;
  
    if (!Fncol) {
      strcpy(g->Message, MSG(NO_DEF_FNCCOL));
      return NULL;
      } // endif Fncol
  
    } // endif Fncol
  
  if (!Picol) {
    // Find default Picol as the last one not equal to Fncol
    for (crp = Qryp->Colresp; crp; crp = crp->Next)
      if (stricmp(Fncol, crp->Name))
        Picol = crp->Name;
  
    if (!Picol) {
      strcpy(g->Message, MSG(NO_DEF_PIVOTCOL));
      return NULL;
      } // endif Picol
  
    } // endif picol
  
  // Prepare the column list
  for (pcrp = &Qryp->Colresp; crp = *pcrp; )
    if (!stricmp(Picol, crp->Name)) {
      Rblkp = crp->Kdata;
      *pcrp = crp->Next;
    } else if (!stricmp(Fncol, crp->Name)) {
      fncrp = crp;
      *pcrp = crp->Next;
    } else
      pcrp = &crp->Next;

  if (!Rblkp) {
    strcpy(g->Message, MSG(NO_DEF_PIVOTCOL));
    return NULL;
  } else if (!fncrp) {
    strcpy(g->Message, MSG(NO_DEF_FNCCOL));
    return NULL;
  } // endif

  // Before calling sort, initialize all
  nblin = Qryp->Nblin;

  Index.Size = nblin * sizeof(int);
  Index.Sub = TRUE;                  // Should be small enough

  if (!PlgDBalloc(g, NULL, Index))
    return NULL;

  Offset.Size = (nblin + 1) * sizeof(int);
  Offset.Sub = TRUE;                 // Should be small enough

  if (!PlgDBalloc(g, NULL, Offset))
    return NULL;

  ndif = Qsort(g, nblin);

  if (ndif < 0)           // error
    return NULL;

  // Allocate the Value used to retieve column names
  if (!(valp = AllocateValue(g, Rblkp->GetType(),
                                Rblkp->GetVlen(),
                                Rblkp->GetPrec())))
    return NULL;

  // Now make the functional columns
  for (int i = 0; i < ndif; i++) {
    if (i) {
      crp = (PCOLRES)PlugSubAlloc(g, NULL, sizeof(COLRES));
      memcpy(crp, fncrp, sizeof(COLRES));
    } else
      crp = fncrp;

    // Get the value that will be the generated column name
    valp->SetValue_pvblk(Rblkp, Pex[Pof[i]]);
    colname = valp->GetCharString(buf);
    crp->Name = (char*)PlugSubAlloc(g, NULL, strlen(colname) + 1);
    strcpy(crp->Name, colname);
    crp->Flag = 1;

    // Add this column
    *pcrp = crp;
    crp->Next = NULL;
    pcrp = &crp->Next;
    } // endfor i

  // We added ndif columns and removed 2 (picol and fncol)
  Qryp->Nbcol += (ndif - 2);
  return Qryp;
  } // end of MakePivotColumns

/***********************************************************************/
/*  PIVAID: Compare routine for sorting pivot column values.           */
/***********************************************************************/
int PIVAID::Qcompare(int *i1, int *i2)
  {
  // TODO: the actual comparison between pivot column result values.
  return Rblkp->CompVal(*i1, *i2);
  } // end of Qcompare

/* --------------- Implementation of the PIVOT classes --------------- */

/***********************************************************************/
/*  PIVOTDEF constructor.                                              */
/***********************************************************************/
  PIVOTDEF::PIVOTDEF(void) 
  {
  Host = User = Pwd = DB = NULL;
  Tabname = Tabsrc = Picol = Fncol = Function = NULL;
  GBdone = Accept = false;
  Port = 0;
  } // end of PIVOTDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values from PIVOT table.        */
/***********************************************************************/
bool PIVOTDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  char *p1, *p2;
  PHC    hc = ((MYCAT*)Cat)->GetHandler();

  if (PRXDEF::DefineAM(g, am, poff))
    return TRUE;

  Tabname = (char*)Tablep->GetName();
  DB = (char*)Tablep->GetQualifier();
  Tabsrc = (char*)Tablep->GetSrc();

  Host = Cat->GetStringCatInfo(g, "Host", "localhost");
  User = Cat->GetStringCatInfo(g, "User", "*");
  Pwd = Cat->GetStringCatInfo(g, "Password", NULL);
  Picol = Cat->GetStringCatInfo(g, "PivotCol", NULL);
  Fncol = Cat->GetStringCatInfo(g, "FncCol", NULL);
  
  // If fncol is like avg(colname), separate Fncol and Function
  if (Fncol && (p1 = strchr(Fncol, '(')) && (p2 = strchr(p1, ')')) &&
      (*Fncol != '"') &&  (!*(p2+1))) {
    *p1++ = '\0'; *p2 = '\0';
    Function = Fncol;
    Fncol = p1;
  } else
    Function = Cat->GetStringCatInfo(g, "Function", "SUM");

  GBdone = Cat->GetBoolCatInfo("Groupby", false);
  Accept = Cat->GetBoolCatInfo("Accept", false);
  Port = Cat->GetIntCatInfo("Port", 3306);
  Desc = (Tabsrc) ? Tabsrc : Tabname;
  return FALSE;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB PIVOTDEF::GetTable(PGLOBAL g, MODE m)
  {
  return new(g) TDBPIVOT(this);
  } // end of GetTable

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBPIVOT class.                              */
/***********************************************************************/
TDBPIVOT::TDBPIVOT(PPIVOTDEF tdp) : TDBPRX(tdp)
  {
  Host = tdp->Host;
  Database = tdp->DB;
  User = tdp->User;
  Pwd = tdp->Pwd;
  Port = tdp->Port;
  Tabname = tdp->Tabname;    // Name of source table
  Tabsrc = tdp->Tabsrc;      // SQL description of source table
  Picol = tdp->Picol;        // Pivot column name
  Fncol = tdp->Fncol;        // Function column name
  Function = tdp->Function;  // Aggregate function name
  Xcolp = NULL;              // To the FNCCOL column
//Xresp = NULL;              // To the pivot result column
//Rblkp = NULL;              // The value block of the pivot column
  Fcolp = NULL;              // To the function column
  Dcolp = NULL;              // To the dump column
  GBdone = tdp->GBdone;
  Accept = tdp->Accept;
  Mult = -1;                // Estimated table size
  N = 0;                    // The current table index
  M = 0;                    // The occurence rank
  FileStatus = 0;           // Logical End-of-File
  RowFlag = 0;              // 0: Ok, 1: Same, 2: Skip
  } // end of TDBPIVOT constructor

/***********************************************************************/
/*  Allocate source column description block.                          */
/***********************************************************************/
PCOL TDBPIVOT::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  PCOL colp;

  if (cdp->GetOffset()) {
    colp = new(g) FNCCOL(cdp, this, cprec, n);

    if (cdp->GetOffset() > 1)
      Dcolp = colp;

  } else
    colp = new(g) SRCCOL(cdp, this, cprec, n);

  return colp;
  } // end of MakeCol

/***********************************************************************/
/*  Find default fonction and pivot columns.                           */
/***********************************************************************/
bool TDBPIVOT::FindDefaultColumns(PGLOBAL g)
  {
  PCOLDEF cdp;
  PTABDEF defp = Tdbp->GetDef();

  if (!Fncol) {
    for (cdp = defp->GetCols(); cdp; cdp = cdp->GetNext())
      if (!Picol || stricmp(Picol, cdp->GetName()))
        Fncol = cdp->GetName();
  
    if (!Fncol) {
      strcpy(g->Message, MSG(NO_DEF_FNCCOL));
      return true;
      } // endif Fncol
  
    } // endif Fncol
  
  if (!Picol) {
    // Find default Picol as the last one not equal to Fncol
    for (cdp = defp->GetCols(); cdp; cdp = cdp->GetNext())
      if (stricmp(Fncol, cdp->GetName()))
        Picol = cdp->GetName();
  
    if (!Picol) {
      strcpy(g->Message, MSG(NO_DEF_PIVOTCOL));
      return true;
      } // endif Picol
  
    } // endif Picol
  
  return false;
  } // end of FindDefaultColumns

/***********************************************************************/
/*  Prepare the source table Query.                                    */
/***********************************************************************/
bool TDBPIVOT::GetSourceTable(PGLOBAL g)
  {
  if (Tdbp)
    return false;             // Already done

  if (!Tabsrc && Tabname) {
    // Get the table description block of this table
    if (!(Tdbp = GetSubTable(g, ((PPIVOTDEF)To_Def)->Tablep, true)))
      return true;

    if (!GBdone) {
      char   *colist;
      PCOLDEF cdp;

      if (FindDefaultColumns(g))
        return true;
  
      // Locate the suballocated colist (size is not known yet)
      *(colist = (char*)PlugSubAlloc(g, NULL, 0)) = 0;
  
      // Make the column list
      for (cdp = To_Def->GetCols(); cdp; cdp = cdp->GetNext())
        if (!cdp->GetOffset())
          strcat(strcat(colist, cdp->GetName()), ", ");
  
      // Add the Pivot column at the end of the list
      strcat(colist, Picol);
  
      // Now we know how much was suballocated
      PlugSubAlloc(g, NULL, strlen(colist));
  
      // Locate the source string (size is not known yet)
      Tabsrc = (char*)PlugSubAlloc(g, NULL, 0);
  
      // Start making the definition
      strcat(strcat(strcpy(Tabsrc, "SELECT "), colist), ", ");
  
      // Make it suitable for Pivot by doing the group by
      strcat(strcat(Tabsrc, Function), "(");
      strcat(strcat(strcat(Tabsrc, Fncol), ") "), Fncol);
      strcat(strcat(Tabsrc, " FROM "), Tabname);
      strcat(strcat(Tabsrc, " GROUP BY "), colist);
  
      if (Tdbp->IsView())     // Until MariaDB bug is fixed
        strcat(strcat(Tabsrc, " ORDER BY "), colist);

      // Now we know how much was suballocated
      PlugSubAlloc(g, NULL, strlen(Tabsrc));
      } // endif !GBdone

  } else if (!Tabsrc) {
    strcpy(g->Message, MSG(SRC_TABLE_UNDEF));
    return true;
  } // endif

  if (Tabsrc) {
    // Get the new table description block of this source table
    PTABLE tablep = new(g) XTAB("whatever", Tabsrc);

    tablep->SetQualifier(Database);

    if (!(Tdbp = GetSubTable(g, tablep, true)))
      return true;

    } // endif Tabsrc

  return false;
  } // end of GetSourceTable

/***********************************************************************/
/*  Make the required pivot columns.                                   */
/***********************************************************************/
bool TDBPIVOT::MakePivotColumns(PGLOBAL g)
  {
  if (!Tdbp->IsView()) {
    // This was not done yet if GBdone is true
    if (FindDefaultColumns(g))
      return true;
  
    // Now it is time to allocate the pivot and function columns
    if (!(Fcolp = Tdbp->ColDB(g, Fncol, 0))) {
      // Function column not found in table                                       
      sprintf(g->Message, MSG(COL_ISNOT_TABLE), Fncol, Tabname);
      return true;
    } else if (Fcolp->InitValue(g))
      return true;

    if (!(Xcolp = Tdbp->ColDB(g, Picol, 0))) {
      // Pivot column not found in table                                       
      sprintf(g->Message, MSG(COL_ISNOT_TABLE), Picol, Tabname);
      return true;
    } else if (Xcolp->InitValue(g))
      return true;

    //  Check and initialize the subtable columns
    for (PCOL cp = Columns; cp; cp = cp->GetNext())
      if (cp->GetAmType() == TYPE_AM_SRC) {
        if (((PSRCCOL)cp)->Init(g))
          return TRUE;

      } else if (cp->GetAmType() == TYPE_AM_FNC)
        if (((PFNCCOL)cp)->InitColumn(g))
          return TRUE;

    } // endif isview

  return false;
  } // end of MakePivotColumns

/***********************************************************************/
/*  Make the required pivot columns for an object view.                */
/***********************************************************************/
bool TDBPIVOT::MakeViewColumns(PGLOBAL g)
  {
  if (Tdbp->IsView()) {
    // Tdbp is a view ColDB cannot be used
    PCOL   colp, cp;
    PTDBMY tdbp;

    if (Tdbp->GetAmType() != TYPE_AM_MYSQL) {
      strcpy(g->Message, "View is not MySQL");
      return true;
    } else
      tdbp = (PTDBMY)Tdbp;

    if (!Fncol && !(Fncol = tdbp->FindFieldColumn(Picol))) {
      strcpy(g->Message, MSG(NO_DEF_FNCCOL));
      return true;
      } // endif Fncol

    if (!Picol && !(Picol = tdbp->FindFieldColumn(Fncol))) {
      strcpy(g->Message, MSG(NO_DEF_PIVOTCOL));
      return true;
      } // endif Picol

    // Now it is time to allocate the pivot and function columns
    if (!(Fcolp = tdbp->MakeFieldColumn(g, Fncol)))
    	return true;

    if (!(Xcolp = tdbp->MakeFieldColumn(g, Picol)))
    	return true;

    //  Check and initialize the subtable columns
    for (cp = Columns; cp; cp = cp->GetNext())
      if (cp->GetAmType() == TYPE_AM_SRC) {
        if ((colp = tdbp->MakeFieldColumn(g, cp->GetName()))) {
          ((PSRCCOL)cp)->Colp = colp;        
          ((PSRCCOL)cp)->To_Val = colp->GetValue();
          cp->AddStatus(BUF_READ);     // All is done here
        } else
    			return true;

      } else if (cp->GetAmType() == TYPE_AM_FNC)
        if (((PFNCCOL)cp)->InitColumn(g))
          return TRUE;

    } // endif isview

  return false;
  } // end of MakeViewColumns

/***********************************************************************/
/*  PIVOT GetMaxSize: returns the maximum number of rows in the table. */
/***********************************************************************/
int TDBPIVOT::GetMaxSize(PGLOBAL g)
  {     
#if  0
  if (MaxSize < 0)
    MaxSize = MakePivotColumns(g);

  return MaxSize;
#endif // 0
  return 10;
  } // end of GetMaxSize

/***********************************************************************/
/*  In this sample, ROWID will be the (virtual) row number,            */
/*  while ROWNUM will be the occurence rank in the multiple column.    */
/***********************************************************************/
int TDBPIVOT::RowNumber(PGLOBAL g, bool b)
  {
  return (b) ? M : N;
  } // end of RowNumber
 
/***********************************************************************/
/*  PIVOT Access Method opening routine.                               */
/***********************************************************************/
bool TDBPIVOT::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, just replace it at its beginning.          */
    /*******************************************************************/
    N = M = 0;
    RowFlag = 0;
    FileStatus = 0;
    return FALSE;
    } // endif use

  if (Mode != MODE_READ) {
    /*******************************************************************/
    /* Currently PIVOT tables cannot be modified.                      */
    /*******************************************************************/
    sprintf(g->Message, MSG(TABLE_READ_ONLY), "PIVOT");
    return TRUE;
    } // endif Mode

  if (To_Key_Col || To_Kindex) {
    /*******************************************************************/
    /* Direct access of PIVOT tables is not implemented yet.           */
    /*******************************************************************/
    strcpy(g->Message, MSG(NO_PIV_DIR_ACC));
    return TRUE;
    } // endif To_Key_Col

  /*********************************************************************/
  /*  Do it here if not done yet (should not be the case).             */
  /*********************************************************************/
  if (GetSourceTable(g))
    return TRUE;
                
  // For tables, columns must be allocated before opening
  if (MakePivotColumns(g))
    return TRUE;

  /*********************************************************************/
  /*  Physically open the object table.                                */
  /*********************************************************************/
	if (Tdbp->OpenDB(g))
		return TRUE;

  /*********************************************************************/
  /*  Make all required pivot columns for object views.                */
  /*********************************************************************/
  return MakeViewColumns(g);
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for PIVOT access method.                    */
/***********************************************************************/
int TDBPIVOT::ReadDB(PGLOBAL g)
  {
  int  rc = RC_OK;
  bool newrow = FALSE;
  PCOL colp;

  if (FileStatus == 2)
    return RC_EF;

  if (FileStatus)
    for (colp = Columns; colp; colp = colp->GetNext())
      if (colp->GetAmType() == TYPE_AM_SRC)
        ((PSRCCOL)colp)->SetColumn();

  // New row, reset all function column values
  for (colp = Columns; colp; colp = colp->GetNext())
    if (colp->GetAmType() == TYPE_AM_FNC)
      colp->GetValue()->Reset();

  /*********************************************************************/
  /*  Now start the multi reading process.                             */
  /*********************************************************************/
  do {
    if (RowFlag != 1) {
      if ((rc = Tdbp->ReadDB(g)) != RC_OK) {
        if (FileStatus && rc == RC_EF) {
          // A prepared row remains to be sent
          FileStatus = 2;
          rc = RC_OK;
          } // endif FileStatus

        break;
        } // endif rc

      for (colp = Tdbp->GetColumns(); colp; colp = colp->GetNext())
        colp->ReadColumn(g);

      for (colp = Columns; colp; colp = colp->GetNext())
        if (colp->GetAmType() == TYPE_AM_SRC)
          if (FileStatus) {
            if (((PSRCCOL)colp)->CompareLast()) {
              newrow = (RowFlag) ? TRUE : FALSE;
              break;
              } // endif CompareLast

          } else
            ((PSRCCOL)colp)->SetColumn();

      FileStatus = 1;
      } // endif RowFlag

    if (newrow) {
      RowFlag = 1;
      break;
    } else
      RowFlag = 2;

    // Look for the column having this header
    for (colp = Columns; colp; colp = colp->GetNext())
      if (colp->GetAmType() == TYPE_AM_FNC) {
        if (((PFNCCOL)colp)->CompareColumn())
          break;

        } // endif AmType

    if (!colp && !(colp = Dcolp)) {
      if (!Accept) {
        strcpy(g->Message, MSG(NO_MATCH_COL));
        return RC_FX;
      } else
        continue;

      } // endif colp

    // Set the value of the matching column from the fonction value
    colp->GetValue()->SetValue_pval(Fcolp->GetValue());
    } while (RowFlag == 2);

  N++;
  return rc;
  } // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for PIVOT access methods.         */
/***********************************************************************/
int TDBPIVOT::WriteDB(PGLOBAL g)
  {
  sprintf(g->Message, MSG(TABLE_READ_ONLY), "PIVOT");
  return RC_FX;
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for PIVOT access methods.            */
/***********************************************************************/
int TDBPIVOT::DeleteDB(PGLOBAL g, int irc)
  {
  sprintf(g->Message, MSG(NO_TABLE_DEL), "PIVOT");
  return RC_FX;
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for PIVOT access method.                   */
/***********************************************************************/
void TDBPIVOT::CloseDB(PGLOBAL g)
  {
  if (Tdbp)
    Tdbp->CloseDB(g);

  } // end of CloseDB

// ------------------------ FNCCOL functions ----------------------------

/***********************************************************************/
/*  FNCCOL public constructor.                                       */
/***********************************************************************/
FNCCOL::FNCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i)
      : COLBLK(cdp, tdbp, i)
  {
  if (cprec) {
    Next = cprec->GetNext();
    cprec->SetNext(this);
  } else {
    Next = tdbp->GetColumns();
    tdbp->SetColumns(this);
  } // endif cprec

  Value = NULL;     // We'll get a new one later
  Hval = NULL;      // The unconverted header value
  Xcolp = NULL;
  } // end of FNCCOL constructor

/***********************************************************************/
/*  FNCCOL initialization function.                                    */
/***********************************************************************/
bool FNCCOL::InitColumn(PGLOBAL g)
{
  // Must have its own value block
  if (InitValue(g))
    return TRUE;

  // Make a value from the column name
  Hval = AllocateValue(g, Name, TYPE_STRING);
  Hval->SetPrec(1);         // Case insensitive

  Xcolp = ((PTDBPIVOT)To_Tdb)->Xcolp;
  AddStatus(BUF_READ);      // All is done here
  return FALSE;
} // end of InitColumn

/***********************************************************************/
/*  CompareColumn: Compare column value with source column value.      */
/***********************************************************************/
bool FNCCOL::CompareColumn(void)
  {
  // Compare the unconverted values
  return Hval->IsEqual(Xcolp->GetValue(), false);
  } // end of CompareColumn

// ------------------------ SRCCOL functions ----------------------------

/***********************************************************************/
/*  SRCCOL public constructor.                                         */
/***********************************************************************/
SRCCOL::SRCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int n)
      : PRXCOL(cdp, tdbp, cprec, n)
  {
  } // end of SRCCOL constructor

/***********************************************************************/
/*  Initialize the column as pointing to the source column.            */
/***********************************************************************/
bool SRCCOL::Init(PGLOBAL g)
  {
  if (PRXCOL::Init(g))
    return true;

  AddStatus(BUF_READ);     // All is done here
  return false;
  } // end of SRCCOL constructor

/***********************************************************************/
/*  SetColumn: have the column value set from the source column.       */
/***********************************************************************/
void SRCCOL::SetColumn(void)
  {
  Value->SetValue_pval(To_Val);
  } // end of SetColumn

/***********************************************************************/
/*  SetColumn: Compare column value with source column value.          */
/***********************************************************************/
bool SRCCOL::CompareLast(void)
  {
  // Compare the unconverted values
  return !Value->IsEqual(To_Val, true);
  } // end of CompareColumn

/* --------------------- End of TabPivot/TabQrs ---------------------- */
